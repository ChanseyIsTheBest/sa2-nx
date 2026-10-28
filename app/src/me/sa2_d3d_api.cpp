// Sonic Adventure 2: the game's Direct3D at its API boundary (the API-level renderer's game-thread side).
//
// The census (sa2_diag_d3d_census, a diagnostic): which Direct3D functions the game really calls, how often, and what
// command-buffer (PM4) packets each call writes. The census hooks themselves are not part of the normal build: they
// are generated for a census build only (scratchpad d3d_census_build.py: out/census/sa2_d3d_census_all.cpp, every
// function in the Direct3D address range that has no hook of its own, added through SA2_EXTRA_SOURCES and
// direct_calls.py --hooked-list), so the normal build's direct calls stay as they are. In the normal build only the
// renderer's own hooks (Draw*, sa2_me_hooks.cpp; the inline-vertex draw below) call the census, and do nothing more
// than a check of the cvar unless it is on.
//
// With the census on, each outermost call (no other census-hooked function below it on the thread) counts and decodes
// the PM4 it wrote: the device's write pointer (+48) before and after the call. When the call runs out of space, the
// command buffer's space function (hooked by the census build) closes the old segment (decoded up to its write
// pointer) and opens the new one. A report every 10 s lists every function called, its calls (outermost and nested), its words
// per call and its packet signatures; sa2_diag_d3d_census_dump names functions whose first calls are logged word by
// word. No callers: the optimised flavour elides LR stores, so ctx.lr is stale (the scratchpad's d3d_entries.py lists
// the static callers).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "cpu/sa2_hot_copies.h"
#include "me/sa2_api.h"
#include "me/sa2_d3d_api.h"
#include "renut_engine/gpu/sa2_renut_yield.h"

REXCVAR_DEFINE_BOOL(sa2_diag_d3d_census, false, "SA2",
                    "Diagnostic: count the Direct3D functions the game calls, with the PM4 packets each call writes "
                    "(a report every 10 s; needs a census build for more than the draw functions). Changes nothing");
REXCVAR_DEFINE_STRING(sa2_diag_d3d_census_dump, "", "SA2",
                      "Diagnostic (with sa2_diag_d3d_census): comma-separated function addresses (hex) whose first 6 "
                      "outermost calls are logged with every PM4 word they wrote");

namespace sa2::d3d {
namespace {

// sa2-nx (2026-10-28, the setter census): the device's first kDeviceBytes (the snapshot Renut's captures take) in
// 64-byte lines.
constexpr uint32_t kDeviceBytes = 13088, kLine = 64, kLines = (kDeviceBytes + kLine - 1) / kLine;
constexpr uint32_t kLineWords = (kLines + 63) / 64;

struct Stats {
  uint64_t calls = 0, nested = 0, words = 0, segments = 0;
  std::map<std::string, uint64_t> packets;  // signature -> count
  uint64_t lines[kLineWords] = {};          // the device lines its calls changed
  uint64_t line_changes = 0;                // lines changed, summed over its calls
  uint64_t tags_set[3] = {}, tags_cleared[3] = {};  // the dirty tags (m_Pending words 2-4) its calls set, cleared
  uint64_t untagged[kLineWords] = {};               // lines changed by its calls that set no tag
  uint64_t other_thread = 0;  // outermost calls on a thread other than the draws': no lines or tags counted for them
                              // (the draw thread's changes meanwhile would be charged to them)
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Stats> g_stats;
std::unordered_map<uint32_t, uint32_t> g_dumps_left;  // function -> calls still to dump
std::atomic<uint32_t> g_device{0};                    // the Direct3D device (r3 of a Draw* call)
auto g_last_report = std::chrono::steady_clock::now();

thread_local int t_depth = 0;
struct Frame {
  uint32_t address;
  uint32_t wptr, limit;  // the segment being decoded: from wptr on
  bool valid, dump;
  std::string words;  // the dump
  uint32_t device;    // the setter census: the device the call started with, and its bytes then
  uint8_t before[kDeviceBytes];
};
thread_local Frame t_frame{};
// The setter census: lines a census call changed since this thread's last draw; per delta chunk offset (/64), how
// often a draw found it changed, and how often none of those calls had changed it.
thread_local uint64_t t_marked[kLineWords] = {};
uint64_t g_chunk_changed[kLines] = {}, g_chunk_inline[kLines] = {};
uint64_t g_draws = 0, g_draw_chunks = 0, g_draw_inline = 0;
// The dirty tags: per tag bit (word 2-4 x 64, bit numbered from the least significant), the lines changed by the calls
// that set it; the tags pending at the current draw (before its capture clears them); per chunk, the inline changes a
// tag pending at the draw covers (a state tag: any of word 2, word 4 or word 3's high half, for the render state
// chunks; a fetch constant's tag, word 3 bit 31 - n, for its own chunks), and per tag bit the draws it was pending at.
uint64_t g_tag_lines[3 * 64][kLineWords] = {};
uint64_t g_tag_draws[3 * 64] = {};
thread_local uint64_t t_draw_tags[3] = {};
uint64_t g_chunk_inline_tagged[kLines] = {};
uint64_t g_draws_state_tag = 0;
// The thread of the draws (the last CensusDrawDone's): a marker's address, unique per thread.
thread_local char t_thread_marker;
std::atomic<const char*> g_draw_thread{nullptr};

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address + (address >= 0xE0000000u ? 0x1000u : 0u), 4);
  return __builtin_bswap32(v);
}

std::string Signature(uint32_t h, const uint8_t* base, uint32_t p) {
  const uint32_t type = h >> 30;
  if (type == 0) {
    const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
    const uint32_t reg = h & 0x7FFF;
    // Registers by group: constants and fetch constants by range, the rest by exact base.
    if (reg >= 0x4000 && reg < 0x4800) return fmt::format("t0 ALU consts {:04X}..+{}", reg & ~0x3Fu, count);
    if (reg >= 0x4800 && reg < 0x4900) return fmt::format("t0 fetch consts {:04X}..+{}", reg & ~0x5u, count);
    return fmt::format("t0 {:04X}+{}{}", reg, count, (h >> 15) & 1 ? " one-reg" : "");
  }
  if (type == 1) return fmt::format("t1 {:03X},{:03X}", h & 0x7FF, (h >> 11) & 0x7FF);
  if (type == 2) return "t2 filler";
  const uint32_t opcode = (h >> 8) & 0x7F;
  if (opcode == 0x10) {  // NOP: the renderer's own markers
    const uint32_t magic = Load32(base, p + 4);
    if (magic == 0x53413244u) return "t3 NOP marker SA2D";
    if (magic == 0x53413246u) return "t3 NOP placeholder SA2F";
    return fmt::format("t3 NOP ({} words)", ((h >> 16) & 0x3FFF) + 1);
  }
  return fmt::format("t3 {:02X}{} ({} words)", opcode, (h & 1) ? " predicated" : "", ((h >> 16) & 0x3FFF) + 1);
}

// The packets in [first, last] (addresses of words; first = the word after the old write pointer). Caller holds
// g_mutex.
void Decode(Stats& s, const uint8_t* base, uint32_t first, uint32_t last) {
  if (last < first) return;
  s.words += (last - first) / 4 + 1;
  if (t_frame.dump) {
    for (uint32_t p = first; p <= last; p += 4) t_frame.words += fmt::format(" {:08X}", Load32(base, p));
    t_frame.words += " |";
  }
  for (uint32_t p = first; p <= last;) {
    const uint32_t h = Load32(base, p);
    const uint32_t type = h >> 30;
    uint32_t words = 1;
    if (type == 0) words = h ? 1 + ((h >> 16) & 0x3FFF) + 1 : 1;
    else if (type == 1) words = 3;
    else if (type == 3) words = 1 + ((h >> 16) & 0x3FFF) + 1;
    if (h) ++s.packets[Signature(h, base, p)];
    p += 4 * words;
  }
}

// The device's flag bytes (+11068..+11072) at each draw: which of Direct3D's draw-time branches SA2 takes.
std::map<uint64_t, uint64_t> g_flags;

void Report() {
  {
    std::string f;
    for (const auto& [k, n] : g_flags) f += fmt::format(" [{:010X}] x{};", k, n);
    REXLOG_INFO("[d3d census] draw-time device flags (+11068..+11072):{}", f);
    g_flags.clear();
  }
  std::vector<std::pair<uint64_t, uint32_t>> order;
  for (const auto& [address, s] : g_stats)
    if (s.calls || s.nested) order.push_back({s.calls * 4 + s.nested, address});
  std::sort(order.rbegin(), order.rend());
  REXLOG_INFO("[d3d census] last 10 s: {} functions called", order.size());
  for (const auto& [rank, address] : order) {
    Stats& s = g_stats[address];
    std::vector<std::pair<uint64_t, std::string>> packets;
    for (const auto& [sig, n] : s.packets) packets.push_back({n, sig});
    std::sort(packets.rbegin(), packets.rend());
    std::string p;
    for (size_t k = 0; k < packets.size() && k < 16; ++k)
      p += fmt::format(" [{}] x{};", packets[k].second, packets[k].first);
    std::string lines, untagged;
    for (uint32_t l = 0; l < kLines; ++l) {
      if (s.lines[l / 64] >> (l % 64) & 1) lines += fmt::format(" +{}", l * kLine);
      if (s.untagged[l / 64] >> (l % 64) & 1) untagged += fmt::format(" +{}", l * kLine);
    }
    REXLOG_INFO("[d3d census] sub_{:08X}: {} calls ({} nested), {:.1f} PM4 words/call, {} segments; device lines "
                "changed {:.2f}/call:{}; packets:{}",
                address, s.calls, s.nested, s.calls ? double(s.words) / double(s.calls) : 0.0, s.segments,
                s.calls ? double(s.line_changes) / double(s.calls) : 0.0, lines.empty() ? " none" : lines, p);
    if (s.tags_set[0] | s.tags_set[1] | s.tags_set[2] | s.tags_cleared[0] | s.tags_cleared[1] | s.tags_cleared[2] ||
        !untagged.empty() || s.other_thread)
      REXLOG_INFO("[d3d census] tags sub_{:08X}: set {:016X} {:016X} {:016X}; cleared {:016X} {:016X} {:016X}; lines "
                  "changed by calls that set no tag:{}; calls on other threads {}",
                  address, s.tags_set[0], s.tags_set[1], s.tags_set[2], s.tags_cleared[0], s.tags_cleared[1],
                  s.tags_cleared[2], untagged.empty() ? " none" : untagged, s.other_thread);
    s = Stats{};
  }
  // The dirty tags: each bit's lines (from the calls that set it) and its draws.
  for (uint32_t b = 0; b < 3 * 64; ++b) {
    std::string lines;
    for (uint32_t l = 0; l < kLines; ++l)
      if (g_tag_lines[b][l / 64] >> (l % 64) & 1) lines += fmt::format(" +{}", l * kLine);
    if (lines.empty() && !g_tag_draws[b]) continue;
    REXLOG_INFO("[d3d census] tag word {} bit {}: pending at {} draws; lines changed by the calls that set it:{}",
                2 + b / 64, b % 64, g_tag_draws[b], lines.empty() ? " none" : lines);
  }
  std::memset(g_tag_lines, 0, sizeof(g_tag_lines));
  std::memset(g_tag_draws, 0, sizeof(g_tag_draws));
  // The setter census at the draws: the chunks the captures found changed, and those no census call had changed.
  if (g_draws) {
    std::vector<std::pair<uint64_t, uint32_t>> inl;
    for (uint32_t l = 0; l < kLines; ++l)
      if (g_chunk_inline[l]) inl.push_back({g_chunk_inline[l], l});
    std::sort(inl.rbegin(), inl.rend());
    std::string list;
    for (const auto& [n, l] : inl)
      list += fmt::format(" +{} x{} (of {}; tagged {});", l * kLine, n, g_chunk_changed[l], g_chunk_inline_tagged[l]);
    REXLOG_INFO("[d3d census] draws {}: {:.2f} chunks changed a draw, {:.2f} of them by no census call (inline "
                "writes); inline by chunk:{}",
                g_draws, double(g_draw_chunks) / double(g_draws), double(g_draw_inline) / double(g_draws),
                list.empty() ? " none" : list);
    REXLOG_INFO("[d3d census] draws with a state tag pending: {} of {}", g_draws_state_tag, g_draws);
    g_draws = g_draw_chunks = g_draw_inline = g_draws_state_tag = 0;
    std::memset(g_chunk_changed, 0, sizeof(g_chunk_changed));
    std::memset(g_chunk_inline, 0, sizeof(g_chunk_inline));
    std::memset(g_chunk_inline_tagged, 0, sizeof(g_chunk_inline_tagged));
  }
}

// The device before the first draw: Direct3D keeps it behind a pointer the title process reads (a check against the
// process type in its release path chooses this one).
uint32_t Device(const uint8_t* base) {
  if (const uint32_t d = g_device.load(std::memory_order_relaxed)) return d;
  const uint32_t holder = Load32(base, 0x8200070Cu);
  if (holder < 0x40000000u || holder >= 0xA0000000u) return 0;
  const uint32_t device = Load32(base, holder);
  return device >= 0x40000000u && device < 0xA0000000u ? device : 0;
}

void ParseDumps() {
  const std::string list = REXCVAR_GET(sa2_diag_d3d_census_dump);
  size_t i = 0;
  while (i < list.size()) {
    size_t j = list.find_first_of(",; ", i);  // ';' too: the test scripts split their settings on ','
    if (j == std::string::npos) j = list.size();
    const std::string item = list.substr(i, j - i);
    if (!item.empty()) g_dumps_left[uint32_t(std::stoul(item, nullptr, 16))] = 6;
    i = j + 1;
  }
}

}  // namespace

void NoteDevice(uint32_t device) { g_device.store(device, std::memory_order_relaxed); }

void NoteDrawFlags(const uint8_t* base, uint32_t device) {
  if (!CensusOn() || !device) return;
  uint64_t k = 0;
  for (uint32_t i = 0; i < 5; ++i) k = (k << 8) | base[device + 11068 + i];
  std::lock_guard lock(g_mutex);
  ++g_flags[k];
}

bool CensusOn() {
  static const bool on = [] {
    const bool v = REXCVAR_GET(sa2_diag_d3d_census);
    if (v) ParseDumps();
    return v;
  }();
  return on;
}

void CensusEnter(uint32_t address, const uint8_t* base) {
  if (!CensusOn()) return;
  if (t_depth++ > 0) {
    std::lock_guard lock(g_mutex);
    ++g_stats[address].nested;
    return;
  }
  const uint32_t device = Device(base);
  t_frame.address = address;
  t_frame.valid = device != 0;
  t_frame.words.clear();
  t_frame.device = device;  // the setter census: the device's bytes before the call
  if (device) std::memcpy(t_frame.before, base + device, kDeviceBytes);
  if (t_frame.valid) {
    t_frame.wptr = Load32(base, device + 48);
    t_frame.limit = Load32(base, device + 52);
  }
  std::lock_guard lock(g_mutex);
  ++g_stats[address].calls;
  auto dump = g_dumps_left.find(address);
  t_frame.dump = dump != g_dumps_left.end() && dump->second > 0;
  if (t_frame.dump) --dump->second;
}

void CensusLeave(const uint8_t* base) {
  if (!CensusOn()) return;
  if (--t_depth > 0) return;
  const uint32_t device = Device(base);
  std::lock_guard lock(g_mutex);
  Stats& s = g_stats[t_frame.address];
  const bool draw_thread = g_draw_thread.load(std::memory_order_relaxed) == &t_thread_marker;
  if (!draw_thread) ++s.other_thread;
  if (t_frame.device && device == t_frame.device && draw_thread) {  // the setter census: the lines the call changed
    const uint8_t* now = base + device;
    uint64_t changed[kLineWords] = {};
    for (uint32_t l = 0; l < kLines; ++l) {
      const uint32_t at = l * kLine, n = std::min(kLine, kDeviceBytes - at);
      if (std::memcmp(now + at, t_frame.before + at, n) == 0) continue;
      s.lines[l / 64] |= uint64_t(1) << (l % 64);
      t_marked[l / 64] |= uint64_t(1) << (l % 64);
      changed[l / 64] |= uint64_t(1) << (l % 64);
      ++s.line_changes;
    }
    // The dirty tags (big-endian words at +16, +24, +32) it set and cleared, and the lines of each tag it set.
    uint64_t set_any = 0;
    for (uint32_t w = 0; w < 3; ++w) {
      uint64_t was, is;
      std::memcpy(&was, t_frame.before + 16 + 8 * w, 8);
      std::memcpy(&is, now + 16 + 8 * w, 8);
      was = __builtin_bswap64(was);
      is = __builtin_bswap64(is);
      const uint64_t set = is & ~was;
      s.tags_set[w] |= set;
      s.tags_cleared[w] |= was & ~is;
      set_any |= set;
      for (uint64_t bits = set; bits; bits &= bits - 1)
        for (uint32_t k = 0; k < kLineWords; ++k) g_tag_lines[w * 64 + __builtin_ctzll(bits)][k] |= changed[k];
    }
    if (!set_any)
      for (uint32_t k = 0; k < kLineWords; ++k) s.untagged[k] |= changed[k] & ~uint64_t(k == 0 ? 1 : 0);  // not line 0
  }
  if (t_frame.valid && device) {
    const uint32_t wptr = Load32(base, device + 48), limit = Load32(base, device + 52);
    if (limit == t_frame.limit && wptr >= t_frame.wptr) {
      Decode(s, base, t_frame.wptr + 4, wptr);
    } else {
      ++s.segments;  // a switch the space function did not see (not hooked in this build)
    }
  }
  if (t_frame.dump)
    REXLOG_INFO("[d3d census] dump sub_{:08X}:{}", t_frame.address, t_frame.words.empty() ? " (no words)" : t_frame.words);
  const auto now = std::chrono::steady_clock::now();
  if (now - g_last_report >= std::chrono::seconds(10)) {
    g_last_report = now;
    Report();
  }
}

void CensusDrawChunk(uint32_t offset, uint32_t size) {
  if (!CensusOn() || !size) return;
  const uint32_t first = offset / kLine, last = std::min((offset + size - 1) / kLine, kLines - 1);
  bool marked = false;
  for (uint32_t l = first; l <= last; ++l) marked |= (t_marked[l / 64] >> (l % 64)) & 1;
  std::lock_guard lock(g_mutex);
  ++g_chunk_changed[first];
  ++g_draw_chunks;
  if (!marked) {
    ++g_chunk_inline[first];
    ++g_draw_inline;
    // Covered by a tag pending at the draw: a state tag for the render state chunks (0x2780-0x2AFF), a fetch
    // constant's for its chunks (0x480 + 24 n).
    const bool state = t_draw_tags[0] || t_draw_tags[2] || (t_draw_tags[1] >> 32);
    bool tagged = state && offset >= 0x2780 && offset < 0x2B00;
    for (uint32_t n = 0; n < 32 && !tagged; ++n) {
      if (!((t_draw_tags[1] >> (31 - n)) & 1)) continue;
      const uint32_t at = 0x480 + 24 * n;
      tagged = at < offset + size && at + 24 > offset;
    }
    if (tagged) ++g_chunk_inline_tagged[first];
  }
}

void CensusDrawTags(uint64_t states, uint64_t fetch, uint64_t misc) {
  if (!CensusOn()) return;
  t_draw_tags[0] = states;
  t_draw_tags[1] = fetch;
  t_draw_tags[2] = misc;
  std::lock_guard lock(g_mutex);
  for (uint32_t w = 0; w < 3; ++w)
    for (uint64_t bits = t_draw_tags[w]; bits; bits &= bits - 1) ++g_tag_draws[w * 64 + __builtin_ctzll(bits)];
  if (states || misc || (fetch >> 32)) ++g_draws_state_tag;
}

void CensusDrawDone(uint32_t device) {
  if (!CensusOn()) return;
  if (device) g_device.store(device, std::memory_order_relaxed);  // Renut's draws: the device of every census snapshot
  g_draw_thread.store(&t_thread_marker, std::memory_order_relaxed);
  std::memset(t_marked, 0, sizeof(t_marked));
  t_draw_tags[0] = t_draw_tags[1] = t_draw_tags[2] = 0;
  std::lock_guard lock(g_mutex);
  ++g_draws;
}

void CensusSegmentBefore(const uint8_t* base) {
  if (!CensusOn() || t_depth == 0 || !t_frame.valid) return;
  const uint32_t device = Device(base);
  if (!device) return;
  std::lock_guard lock(g_mutex);
  Stats& s = g_stats[t_frame.address];
  const uint32_t wptr = Load32(base, device + 48);
  if (Load32(base, device + 52) == t_frame.limit && wptr >= t_frame.wptr) Decode(s, base, t_frame.wptr + 4, wptr);
  ++s.segments;
}

void CensusSegmentAfter(const uint8_t* base) {
  if (!CensusOn() || t_depth == 0 || !t_frame.valid) return;
  const uint32_t device = Device(base);
  if (!device) return;
  t_frame.wptr = Load32(base, device + 48);
  t_frame.limit = Load32(base, device + 52);
}

}  // namespace sa2::d3d

// The draw path with inline vertices (a localized copy, SA2_COPY_FOR_HOOK in cpu/sa2_hot_guest.cpp).
REX_EXTERN(__imp__sub_82565758);
REX_HOOK_RAW(sub_82565758) {
  SA2_RENUT_YIELD_SHARED(sub_82565758)  // sa2_renderer = "renut"
  if (!sa2::d3d::CensusOn()) [[likely]] {
    if (sa2::api::BeginVertices(ctx, base)) return;  // sa2_api_draws
    sa2::hot::Copy_82565758(ctx, base);
    return;
  }
  sa2::d3d::NoteDevice(ctx.r3.u32);
  sa2::d3d::CensusEnter(0x82565758u, base);
  sa2::hot::Copy_82565758(ctx, base);
  sa2::d3d::CensusLeave(base);
}
