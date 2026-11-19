// banjo-nx - the game's Direct3D at its API boundary: the census (banjo_diag_d3d_census, a diagnostic).
//
// Ported from sa2-nx's app/src/me/sa2_d3d_api.cpp (GPL-3.0, same project family; docs/api-renderer.md there is the
// method). Which Direct3D functions the game really calls, how often, through which device object and thread, and what
// command-buffer (PM4) packets each outermost call writes. What is different in Banjo:
//
//   * four Direct3D "device" objects on four threads (the engine records most draws into command buffers of its own),
//     so everything is kept per device, and a call's device is its r3 when that is a known device, else the device the
//     calling thread used last;
//   * Direct3D is not one address range here (the game was linked with profile-guided function order), so the census
//     build hooks the command-buffer writers a static scan finds (tools/d3d/d3d_writers.py) and the named entry
//     points, not a range;
//   * the draw functions' own facts are counted too (NoteDraw): the device's flag bytes and which parts of the
//     draw-time state flush a draw runs. They size the native draw path (docs/d3d-native.md).
//
// The census hooks are not part of the normal build: tools/d3d/d3d_census_build.py writes them into
// app/src/me/census/banjo_d3d_census_hooks.inc (included by banjo_me_hooks.cpp when it exists) and re-runs the
// post-codegen steps, so the callers of those functions go through the hooks; --restore removes the file again. In the
// normal build only the renderer's own hooks (banjo_me_hooks.cpp) call the census, and do nothing more than read one
// byte unless the key is on.
//
// With the census on, each outermost call (no other census-hooked function below it on the thread) counts and decodes
// the PM4 it wrote: the device's write pointer (+48) before and after the call. When the call runs out of space, the
// space functions (hooked by the census build) close the old segment and open the new one. A report every 10 s.
//
// Header-only and included by exactly one source file (banjo_me_hooks.cpp): a new source file would mean a CMake
// re-run, which rebuilds the code generator and with it every generated file.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "cpu/banjo_hot_common.h"

REXCVAR_DEFINE_BOOL(banjo_diag_d3d_census, false, "Banjo",
                    "Diagnostic: count the Direct3D functions the game calls, per device object and thread, with the "
                    "PM4 packets each call writes (a report every 10 s; needs a census build, "
                    "tools/d3d/d3d_census_build.py, for more than the draw functions). Changes nothing");
REXCVAR_DEFINE_STRING(banjo_diag_d3d_census_dump, "", "Banjo",
                      "Diagnostic (with banjo_diag_d3d_census): function addresses (hex, separated by ';') whose first "
                      "6 outermost calls are logged with every PM4 word they wrote");

extern std::atomic<uint64_t> g_banjo_game_frames;

namespace banjo::d3d {

// Banjo's Direct3D device (this XDK build): the fields the census reads. docs/d3d-native.md has the whole table.
namespace dev {
inline constexpr uint32_t kDirtyVsConsts = 0, kDirtyPsConsts = 8, kDirtyGroups = 16, kDirtyFetch = 24, kDirtyMisc = 32,
                          kDirtyRb = 40;
inline constexpr uint32_t kWptr = 48, kLimit = 52, kSoftLimit = 56;
inline constexpr uint32_t kFlags = 10940;  // five flag bytes, +10940..+10944
inline constexpr uint32_t kIndexBuffer = 12436;
}  // namespace dev

namespace census {

inline constexpr uint32_t kMaxDevices = 8;

struct Stats {
  uint64_t calls = 0, nested = 0, words = 0, segments = 0;
  std::map<std::string, uint64_t> packets;  // signature -> count
};

inline std::mutex g_mutex;
inline std::map<uint64_t, Stats> g_stats;                       // (device index << 32 | function) -> counts
inline std::unordered_map<uint32_t, uint32_t> g_dumps_left;     // function -> calls still to dump
inline std::atomic<uint32_t> g_devices[kMaxDevices] = {};       // the devices seen (r3 of a Draw* call)
inline std::map<uint64_t, uint64_t> g_device_threads;           // (device index << 32 | thread index) -> calls
inline std::map<uint64_t, uint64_t> g_flags[kMaxDevices];       // the five flag bytes at a draw -> draws
inline std::map<uint32_t, uint64_t> g_paths[kMaxDevices][3];    // per draw function: the flush's path bits -> draws
inline uint64_t g_blocks[kMaxDevices][4];                       // dirty VS blocks, PS blocks, fetch bits, group bits
inline std::atomic<uint32_t> g_thread_count{0};
inline auto g_last_report = std::chrono::steady_clock::now();
inline const auto g_start = std::chrono::steady_clock::now();
inline uint64_t g_last_frames = 0;

struct Frame {
  uint32_t address = 0;
  uint32_t device = 0;
  uint32_t wptr = 0, limit = 0;  // the segment being decoded: from wptr on
  bool valid = false, dump = false;
  std::string words;  // the dump
};
// Census only (never touched with the key off): thread_local is a function call on the Switch.
inline thread_local int t_depth = 0;
inline thread_local Frame t_frame{};
inline thread_local uint32_t t_device = 0;
inline thread_local uint32_t t_thread = 0xFFFFFFFFu;

inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address + ::banjo::hot::PhysOff(address), 4);
  return __builtin_bswap32(v);
}
inline uint64_t Load64(const uint8_t* base, uint32_t address) {
  return (uint64_t(Load32(base, address)) << 32) | Load32(base, address + 4);
}

inline int DeviceIndex(uint32_t device) {
  for (uint32_t i = 0; i < kMaxDevices; ++i)
    if (g_devices[i].load(std::memory_order_relaxed) == device) return int(i);
  return -1;
}

inline uint32_t ThreadIndex() {
  if (t_thread == 0xFFFFFFFFu) t_thread = g_thread_count.fetch_add(1, std::memory_order_relaxed);
  return t_thread;
}

inline std::string Signature(uint32_t h, const uint8_t* base, uint32_t p) {
  const uint32_t type = h >> 30;
  if (type == 0) {
    const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
    const uint32_t reg = h & 0x7FFF;
    if (reg >= 0x4000 && reg < 0x4800) return fmt::format("t0 ALU consts {:04X}..", reg & 0x4400u);
    if (reg >= 0x4800 && reg < 0x4900) return "t0 fetch consts";
    return fmt::format("t0 {:04X}+{}{}", reg, count, (h >> 15) & 1 ? " one-reg" : "");
  }
  if (type == 1) return fmt::format("t1 {:03X},{:03X}", h & 0x7FF, (h >> 11) & 0x7FF);
  if (type == 2) return "t2 filler";
  const uint32_t opcode = (h >> 8) & 0x7F;
  if (opcode == 0x10) {  // NOP: the renderer's own markers
    if (Load32(base, p + 4) == 0x424A4E44u) return "t3 NOP marker BJND";
    return "t3 NOP";
  }
  return fmt::format("t3 {:02X}{}", opcode, (h & 1) ? " predicated" : "");
}

// The packets in [first, last] (addresses of words; first = the word after the old write pointer). Caller holds
// g_mutex.
inline void Decode(Stats& s, const uint8_t* base, uint32_t first, uint32_t last) {
  if (last < first || last - first > 0x100000u) return;
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

inline void Report() {
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
  const uint64_t frames = g_banjo_game_frames.load(std::memory_order_relaxed);
  const uint64_t swaps = frames - g_last_frames;
  g_last_frames = frames;
  REXLOG_INFO("[d3d census] report at t={:.0f} s: {} Swaps in the last 10 s, {} threads have called Direct3D", t, swaps,
              g_thread_count.load(std::memory_order_relaxed));
  for (uint32_t d = 0; d < kMaxDevices; ++d) {
    const uint32_t device = g_devices[d].load(std::memory_order_relaxed);
    if (!device) continue;
    std::string threads, flags;
    for (auto& [k, n] : g_device_threads)
      if (uint32_t(k >> 32) == d && n) {
        threads += fmt::format(" T{} x{};", uint32_t(k), n);
        n = 0;
      }
    for (const auto& [k, n] : g_flags[d]) flags += fmt::format(" [{:010X}] x{};", k, n);
    g_flags[d].clear();
    REXLOG_INFO("[d3d census] device D{} = {:08X}: calls by thread:{} | draw-time flags (+10940..+10944):{} | dirty per "
                "draw call (sums): VS blocks {}, PS blocks {}, fetch bits {}, group bits {}",
                d, device, threads.empty() ? " none" : threads, flags.empty() ? " no draws" : flags, g_blocks[d][0],
                g_blocks[d][1], g_blocks[d][2], g_blocks[d][3]);
    for (uint32_t k = 0; k < 4; ++k) g_blocks[d][k] = 0;
    static const char* const kDrawNames[3] = {"DrawIndexedVertices", "DrawVertices", "DrawVerticesUP"};
    for (uint32_t f = 0; f < 3; ++f) {
      if (g_paths[d][f].empty()) continue;
      std::vector<std::pair<uint64_t, uint32_t>> order;
      uint64_t total = 0;
      for (const auto& [bits, n] : g_paths[d][f]) {
        order.push_back({n, bits});
        total += n;
      }
      std::sort(order.rbegin(), order.rend());
      std::string p;
      for (size_t k = 0; k < order.size() && k < 20; ++k) p += fmt::format(" {:05X} x{};", order[k].second, order[k].first);
      // Bits: 0 VS consts, 1 PS consts, 2 shader load, 3 re-send (+40), 4 group 2200, 5 group 2180, 6 group 2000,
      // 7 group 2100, 8 fetch consts, 9 group 2280, 10 group 2300, 11 group 2380, 12 the 2388 block, 13 group 4900,
      // 14 more than 65535 vertices, 15 32-bit indices, 16 tiling flag (+10940 bit 0), 17 recording flags (+10940 &
      // 0x80), 18 any group mask set.
      REXLOG_INFO("[d3d census] D{} {}: {} calls, {} flush paths; paths (bits in banjo_d3d_census.h):{}", d,
                  kDrawNames[f], total, order.size(), p);
      g_paths[d][f].clear();
    }
  }
  std::vector<std::pair<uint64_t, uint64_t>> order;
  for (const auto& [key, s] : g_stats)
    if (s.calls || s.nested) order.push_back({s.calls * 4 + s.nested, key});
  std::sort(order.rbegin(), order.rend());
  REXLOG_INFO("[d3d census] last 10 s: {} function/device pairs called", order.size());
  for (const auto& [rank, key] : order) {
    Stats& s = g_stats[key];
    std::vector<std::pair<uint64_t, std::string>> packets;
    for (const auto& [sig, n] : s.packets) packets.push_back({n, sig});
    std::sort(packets.rbegin(), packets.rend());
    std::string p;
    for (size_t k = 0; k < packets.size() && k < 14; ++k) p += fmt::format(" [{}] x{};", packets[k].second, packets[k].first);
    const uint32_t d = uint32_t(key >> 32);
    REXLOG_INFO("[d3d census] fn {:08X} {}: {} calls ({} nested), {} PM4 words, {:.1f} words/call, {} segments; "
                "packets:{}",
                uint32_t(key), d == 0xFF ? std::string("D?") : fmt::format("D{}", d), s.calls, s.nested, s.words,
                s.calls ? double(s.words) / double(s.calls) : 0.0, s.segments, p);
    s = Stats{};
  }
}

inline void ParseDumps() {
  const std::string list = REXCVAR_GET(banjo_diag_d3d_census_dump);
  size_t i = 0;
  while (i < list.size()) {
    size_t j = list.find_first_of(",; ", i);
    if (j == std::string::npos) j = list.size();
    const std::string item = list.substr(i, j - i);
    if (!item.empty()) g_dumps_left[uint32_t(std::stoul(item, nullptr, 16))] = 6;
    i = j + 1;
  }
}

// 0 not read yet, 1 off, 2 on: one byte for the hooks of the normal build.
inline uint8_t g_on = 0;
inline uint8_t Init() {
  const bool v = REXCVAR_GET(banjo_diag_d3d_census);
  if (v) ParseDumps();
  g_on = v ? 2 : 1;
  return g_on;
}

}  // namespace census

[[gnu::always_inline]] inline bool CensusOn() {
  const uint8_t s = census::g_on;
  if (s == 1) [[likely]] return false;
  return (s ? s : census::Init()) == 2;
}

// A device, proven by a Draw* call's r3.
inline void NoteDevice(uint32_t device) {
  using namespace census;
  t_device = device;
  if (DeviceIndex(device) >= 0) return;
  std::lock_guard lock(g_mutex);
  if (DeviceIndex(device) >= 0) return;
  for (uint32_t i = 0; i < kMaxDevices; ++i)
    if (!g_devices[i].load(std::memory_order_relaxed)) {
      g_devices[i].store(device, std::memory_order_relaxed);
      REXLOG_INFO("[d3d census] device D{} = {:08X} (first drawn on by thread T{})", i, device, ThreadIndex());
      return;
    }
}

// At a draw call's entry (which: 0 DrawIndexedVertices, 1 DrawVertices, 2 DrawVerticesUP): the device's flag bytes and
// the parts of the draw-time state flush this call will run (the tests of the draw functions' inlined FlushState).
inline void NoteDraw(const uint8_t* base, uint32_t device, uint32_t which, uint32_t count) {
  using namespace census;
  if (!CensusOn() || !device) return;
  NoteDevice(device);
  const int d = DeviceIndex(device);
  if (d < 0) return;
  uint64_t k = 0;
  for (uint32_t i = 0; i < 5; ++i) k = (k << 8) | base[device + dev::kFlags + i];  // (devices are below 0xE0000000)
  const uint64_t vs = Load64(base, device + dev::kDirtyVsConsts), ps = Load64(base, device + dev::kDirtyPsConsts),
                 groups = Load64(base, device + dev::kDirtyGroups), fetch = Load64(base, device + dev::kDirtyFetch),
                 misc = Load64(base, device + dev::kDirtyMisc), rb = Load64(base, device + dev::kDirtyRb);
  uint32_t bits = 0;
  if (vs) bits |= 1u << 0;
  if (ps) bits |= 1u << 1;
  if (groups) {
    bits |= 1u << 18;
    if (uint32_t(groups) & 0x001E0000u) bits |= 1u << 2;
    if (rb & groups) bits |= 1u << 3;  // (the shader load can add group bits: a lower bound)
    if (groups & 0xFFFull) bits |= 1u << 4;
    if (uint32_t(groups) & 0x0001F000u) bits |= 1u << 5;
    if (groups & (0xFFFFull << 42)) bits |= 1u << 6;
    if (groups & 0x000003FFFFE00000ull) bits |= 1u << 7;
  }
  if (fetch) {
    if (fetch & 0xFFFFFFFFull) bits |= 1u << 8;
    if (fetch & (0x1FFFFFull << 34)) bits |= 1u << 9;
  }
  if (misc) {
    if (misc & 0x3FFFFFFFFFull) bits |= 1u << 10;
    if (misc & (0xFFull << 38)) bits |= 1u << 11;
    if (misc & (0x3Full << 49)) bits |= 1u << 12;
    if (misc & (1ull << 56)) bits |= 1u << 13;
  }
  if (count > 65535) bits |= 1u << 14;
  if (which == 0) {
    const uint32_t ib = Load32(base, device + dev::kIndexBuffer);
    if (ib >= 0x40000000u && (Load32(base, ib) & 0x80000000u)) bits |= 1u << 15;
  }
  if (base[device + dev::kFlags] & 0x01) bits |= 1u << 16;
  if (base[device + dev::kFlags] & 0x80) bits |= 1u << 17;
  std::lock_guard lock(g_mutex);
  ++g_flags[d][k];
  ++g_paths[d][which < 3 ? which : 0][bits];
  g_blocks[d][0] += uint64_t(__builtin_popcountll(vs));
  g_blocks[d][1] += uint64_t(__builtin_popcountll(ps));
  g_blocks[d][2] += uint64_t(__builtin_popcountll(fetch));
  g_blocks[d][3] += uint64_t(__builtin_popcountll(groups));
}

inline void CensusEnter(uint32_t address, uint32_t r3, const uint8_t* base) {
  using namespace census;
  if (!CensusOn()) return;
  if (t_depth++ > 0) {
    std::lock_guard lock(g_mutex);
    const int d = DeviceIndex(t_frame.device);
    ++g_stats[(uint64_t(d < 0 ? 0xFF : d) << 32) | address].nested;
    return;
  }
  // The call's device: r3 when it is one, else the one this thread used last, else the first one.
  uint32_t device = DeviceIndex(r3) >= 0 ? r3 : t_device;
  if (!device) device = g_devices[0].load(std::memory_order_relaxed);
  t_frame.address = address;
  t_frame.device = device;
  t_frame.valid = device != 0;
  t_frame.words.clear();
  if (t_frame.valid) {
    t_frame.wptr = Load32(base, device + dev::kWptr);
    t_frame.limit = Load32(base, device + dev::kLimit);
  }
  const int d = DeviceIndex(device);
  std::lock_guard lock(g_mutex);
  ++g_stats[(uint64_t(d < 0 ? 0xFF : d) << 32) | address].calls;
  if (d >= 0) ++g_device_threads[(uint64_t(d) << 32) | ThreadIndex()];
  auto dump = g_dumps_left.find(address);
  t_frame.dump = dump != g_dumps_left.end() && dump->second > 0;
  if (t_frame.dump) --dump->second;
}

inline void CensusLeave(const uint8_t* base) {
  using namespace census;
  if (!CensusOn()) return;
  if (--t_depth > 0) return;
  const uint32_t device = t_frame.device;
  const int d = DeviceIndex(device);
  std::lock_guard lock(g_mutex);
  Stats& s = g_stats[(uint64_t(d < 0 ? 0xFF : d) << 32) | t_frame.address];
  if (t_frame.valid && device) {
    const uint32_t wptr = Load32(base, device + dev::kWptr), limit = Load32(base, device + dev::kLimit);
    if (limit == t_frame.limit && wptr >= t_frame.wptr) {
      Decode(s, base, t_frame.wptr + 4, wptr);
    } else {
      ++s.segments;  // a switch the space functions did not show (not hooked in this build)
    }
  }
  if (t_frame.dump)
    REXLOG_INFO("[d3d census] dump fn {:08X} D{}:{}", t_frame.address, d, t_frame.words.empty() ? " (no words)" : t_frame.words);
  const auto now = std::chrono::steady_clock::now();
  if (now - g_last_report >= std::chrono::seconds(10)) {
    g_last_report = now;
    Report();
  }
}

// Around Direct3D's space functions (a segment switch inside a call): the old segment's packets are decoded before,
// the new segment is followed after.
inline void CensusSegmentBefore(const uint8_t* base) {
  using namespace census;
  if (!CensusOn() || t_depth == 0 || !t_frame.valid) return;
  const uint32_t device = t_frame.device;
  const int d = DeviceIndex(device);
  std::lock_guard lock(g_mutex);
  Stats& s = g_stats[(uint64_t(d < 0 ? 0xFF : d) << 32) | t_frame.address];
  const uint32_t wptr = Load32(base, device + dev::kWptr);
  if (Load32(base, device + dev::kLimit) == t_frame.limit && wptr >= t_frame.wptr) Decode(s, base, t_frame.wptr + 4, wptr);
  ++s.segments;
}

inline void CensusSegmentAfter(const uint8_t* base) {
  using namespace census;
  if (!CensusOn() || t_depth == 0 || !t_frame.valid) return;
  t_frame.wptr = Load32(base, t_frame.device + dev::kWptr);
  t_frame.limit = Load32(base, t_frame.device + dev::kLimit);
}

}  // namespace banjo::d3d

// The census build's hooks (generated, see above): the original runs unchanged.
#define BANJO_D3D_CENSUS_HOOK(ADDR)                           \
  REX_EXTERN(__imp__sub_##ADDR);                              \
  REX_HOOK_RAW(sub_##ADDR) {                                  \
    banjo::d3d::CensusEnter(0x##ADDR##u, ctx.r3.u32, base);   \
    __imp__sub_##ADDR(ctx, base);                             \
    banjo::d3d::CensusLeave(base);                            \
  }
#define BANJO_D3D_CENSUS_SEGMENT_HOOK(ADDR)                   \
  REX_EXTERN(__imp__sub_##ADDR);                              \
  REX_HOOK_RAW(sub_##ADDR) {                                  \
    banjo::d3d::CensusSegmentBefore(base);                    \
    __imp__sub_##ADDR(ctx, base);                             \
    banjo::d3d::CensusSegmentAfter(base);                     \
  }
