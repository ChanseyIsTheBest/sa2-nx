// banjo-nx - Direct3D's draw path as native code (CPU enhancements banjo_d3d_native_draw_indexed / _draw_vertices).
//
// The method is sa2-nx's native Direct3D layer (app/src/me/sa2_api_draw.cpp, sa2_api_internal.h, docs/api-renderer.md
// there; GPL-3.0, same project family): the draw entry points written from Direct3D's own instructions, with helpers
// that mirror PowerPC's rotate-and-mask so every line can be held against the listing (tools/d3d/ppcfn.py), and the
// Direct3D device structure left exactly as the game sees it. docs/d3d-native.md has Banjo's device layout, the census
// and the measurements.
//
// What is different from sa2-nx, and why:
//
//   * THE OUTPUT IS THE SAME PM4, byte for byte, in Direct3D's own command buffer (sa2-nx writes host records for its
//     ring thread instead). Banjo's engine records most draws into command buffers of its own on several threads
//     (one Direct3D "device" object each), the device runs those once per EDRAM tile, and Direct3D patches a predicated
//     draw's tile mask in the command buffer after the draw call. Identical packets keep all of that as it is, and the
//     ring side of a record transport is what banjo_direct_state already is (measured: not faster on the PC ring).
//   * So any part of the path can stay Direct3D's own code: the native function calls the recompiled helper with the
//     registers the original passes at that point (the draw-time shader load, the two rare re-send blocks). They write
//     their packets into the same command buffer; what comes back is read again from the device, as the original does.
//   * Everything a thread needs is in its own guest context (PPCContext::host_counters): no thread_local (a function
//     call on the Switch), no counter shared between the four drawing threads on the usual path.
//
// The native part: the draw functions' inlined FlushState (which dirty masks, in which order, with which register
// numbers), the writers of the register runs (Direct3D's sub-functions for float-constant blocks, register words and
// fetch constants, as one inlined loop each, guest order to guest order, no byte swap) and the draw packets, with and
// without the predicated-tiling wrapper. Checked BEFORE anything is changed, the original runs instead for: more
// than 65535 vertices (the chunk loop), a command buffer window that is not plainly usable, no index buffer. Where a
// run might not fit the command buffer segment, that one block is written by Direct3D's own writer (which calls for
// space); where the draw packets need space or a new tile-extents block, the original draw function finishes the call
// (every dirty mask is zero by then, so it goes straight to its draw part: the same instructions from there on).
//
// THE CHECK (Checked below). For the first banjo_hot_guard_calls calls of each thread, one call in
// banjo_hot_guard_period after that, and every call with banjo_cpu_verify: the native code runs on a private COPY of
// the device whose command buffer is a scratch area (the real write pointer's alignment phase, far limits), then the
// ORIGINAL runs on the same copy from the same bytes and into the same scratch area, and the two are compared byte
// for byte: the packets, the whole device structure (no field left out: nothing else can write a private copy), and
// the guest memory a draw writes outside the device (captured after each run and put back: the vertex shader's
// binding states, descriptors and microcode, the declaration id and Direct3D's id counter, the tile-extents entry).
// Then the original runs for real: a checked call always gets Direct3D's own result. sa2-nx runs the original on a
// copy and the native code for real, and compares the copy with the real device; in Banjo that comparison meets the
// fields other threads write into the real device (the vertical-blank interrupt handler's counters, the inline-data
// allocator's critical section, the GPU's write-backs: found by the first version of this check), which two private
// copies do not have. A difference is logged, counted (me::native::VerifyDifference) and switches that entry point
// off. One checked call in eight makes the native run send every block through Direct3D's own writers, which is how
// the "does not fit" branch is compared. Not compared: a call the native run declines.
//
// Registers: the originals leave values in r3-r12 (no caller reads one after the call: sa2-nx's
// tools/call_site_liveness.py over the 24 direct call sites); the native code writes r3-r6 only where it calls a
// recompiled helper, which is a subset of what the original writes, and puts r3-r7 back to the call's arguments
// before it hands the call to the original. The original's stack frame is linked the same way
// (the back chain word), and its helpers get the same stack pointer.
//
// banjo_d3d_records_native (step 5 of docs/d3d-full-replacement.md): with banjo_d3d_records on, the hook hands the
// native code its device's staging area (RecordOps) and the native code writes the record's operations of every
// packet as it writes the packet: the register runs from the shadows (host order), the draw packets and the tile
// wrapper; what Direct3D's own helpers write in between (the shader load, the re-send block, a block that did not fit)
// is decoded from the command buffer right after the helper returns (banjo_records.h). The packets are still
// written: the decoder never reads them, but they are the fallback (a command buffer run again after its record left
// the ring is parsed) and what the producer's check compares the operations with (SamePiece: the first records, every
// record in verify mode). Anything the operations cannot describe (a helper wrote a barrier or changed segment, the
// original finishes the call) leaves `cur` null, and the hook decodes the region from the packets instead.
//
// Header-only and included by exactly one source file (banjo_me_hooks.cpp): see banjo_d3d_census.h.
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "cpu/banjo_enhancements.h"
#include "cpu/banjo_hot_common.h"
#include "me/banjo_records.h"
#include "me/me_verify.h"

REXCVAR_DEFINE_BOOL(banjo_d3d_native_draw_indexed, true, "Banjo/Enhancements",
                    "Native Direct3D draw path (CPU enhancement): D3DDevice_DrawIndexedVertices with its draw-time "
                    "state flush as native code that writes the same PM4 into Direct3D's command buffer. Checked "
                    "against the original on a device copy (first calls of each thread, then 1 in "
                    "banjo_hot_guard_period; every call with banjo_cpu_verify)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_d3d_native_draw_vertices, true, "Banjo/Enhancements",
                    "Native Direct3D draw path (CPU enhancement): the same for D3DDevice_DrawVertices")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, banjo_cpu_verify);
REXCVAR_DECLARE(int32_t, banjo_hot_guard_calls);
REXCVAR_DECLARE(int32_t, banjo_hot_guard_period);

// Direct3D's own code the native path calls. Hooked ones through their hooks (the shader load: the renderer notes the
// shader objects there; the three writers: their hand-written natives), the others directly.
REX_EXTERN(sub_821FAF10);
REX_EXTERN(sub_821FBC70);
REX_EXTERN(sub_821FC1C8);
REX_EXTERN(sub_821F9BB8);
REX_EXTERN(__imp__sub_82239418);
REX_EXTERN(__imp__sub_821DD898);
REX_EXTERN(__imp__sub_8263F358);
REX_EXTERN(__imp__sub_821FB880);

namespace banjo::d3d {
namespace native {

// ---------------------------------------------------------------------------------------------------------------------
// PowerPC rotate-and-mask, IBM bit numbering (bit 0 = most significant). From sa2-nx's sa2_api_internal.h.
constexpr uint32_t Mask32(int mb, int me) {
  const uint32_t a = 0xFFFFFFFFu >> mb, b = 0xFFFFFFFFu << (31 - me);
  return mb <= me ? (a & b) : (a | b);
}
constexpr uint32_t Rlwinm(uint32_t s, int sh, int mb, int me) { return std::rotl(s, sh) & Mask32(mb, me); }
constexpr uint32_t Rlwimi(uint32_t a, uint32_t s, int sh, int mb, int me) {
  const uint32_t m = Mask32(mb, me);
  return (std::rotl(s, sh) & m) | (a & ~m);
}
constexpr uint64_t Rldicr(uint64_t s, int sh, int me) { return std::rotl(s, sh) & (~0ull << (63 - me)); }

// ---------------------------------------------------------------------------------------------------------------------
// Banjo's Direct3D device (this XDK build; 24064 bytes): the fields the draw path reads and writes.
namespace dev {
inline constexpr uint32_t kDirtyVsConsts = 0, kDirtyPsConsts = 8, kDirtyGroups = 16, kDirtyFetch = 24, kDirtyMisc = 32,
                          kDirtyRb = 40;  // groups re-sent by Direct3D's re-send block
inline constexpr uint32_t kWptr = 48, kLimit = 52, kSoftLimit = 56;  // the command buffer segment
inline constexpr uint32_t kShadowFetch = 1152, kShadowVsConsts = 1920, kShadowPsConsts = 6016, kShadowBoolLoop = 10112;
inline constexpr uint32_t kShadow2000 = 10368, kShadow2100 = 10444, kShadow2180 = 10528, kShadow2200 = 10548,
                          kShadow2280 = 10596, kShadow2300 = 10680, kShadow2380 = 10832;
inline constexpr uint32_t kReg2203 = 10560;
inline constexpr uint32_t kFenceCompleted = 10896;  // -> the GPU's last completed fence
inline constexpr uint32_t kFlags = 10940;           // +10940..+10944
inline constexpr uint32_t kVertexDecl = 11820;
inline constexpr uint32_t kVizTable = 12122, kVizIndex = 12187;
inline constexpr uint32_t kIndexBuffer = 12436;
inline constexpr uint32_t kPixelShader = 12692, kVertexShader = 12696;
inline constexpr uint32_t kExtentsNext = 13256, kExtentsEnd = 13260;  // the tile-extents list (header at +13248)
inline constexpr uint32_t kSize = 24064;
}  // namespace dev

// Vertices per primitive and overlap, per primitive type (8 bytes each): only for draws of more than 65535 vertices,
// which run the original.
// Direct3D's counter of vertex declaration ids (its binding check bumps it): a region the check captures.
inline constexpr uint32_t kGlobalDeclIds = 0x82FB0130u;

// ---------------------------------------------------------------------------------------------------------------------
// Guest memory. The device is below the 0xE0000000 range (checked once per call), so its fields are at base + address;
// command buffers and resources go through Raw (the Windows host's offset of that range).
inline constexpr uint64_t kDeviceLow = 0x10000ull, kDeviceHigh = 0xE0000000ull - 0x10000ull;
using ::banjo::hot::PhysOff;
using ::banjo::hot::Raw;

[[gnu::always_inline]] inline uint32_t L32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
[[gnu::always_inline]] inline uint64_t L64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return __builtin_bswap64(v);
}
[[gnu::always_inline]] inline void S32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
[[gnu::always_inline]] inline void S64(uint8_t* p, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(p, &v, 8);
}
struct B16 {
  uint8_t b[16];
};
[[gnu::always_inline]] inline void Copy16(uint8_t* d, const uint8_t* s) {
  B16 v;
  std::memcpy(v.b, s, 16);
  std::memcpy(d, v.b, 16);
}
// n bytes, a multiple of four from 4 to 64, between ranges that do not overlap: overlapping 16-, 8- or 4-byte moves,
// all loads before all stores, no loop (masseffect-nx's small copy, as banjo_hand_natives.h has it). The compiler
// turned a word loop here into a vector loop with forty instructions of set-up, for runs that are one to three words
// long (seen in the Switch compiler's output, not on the PC).
[[gnu::always_inline]] inline void CopyWordsSmall(uint8_t* d, const uint8_t* s, uint32_t n) {
  if (n >= 16) {
    B16 a, b, c, e;
    std::memcpy(a.b, s, 16);
    std::memcpy(e.b, s + n - 16, 16);
    if (n > 32) {
      std::memcpy(b.b, s + 16, 16);
      std::memcpy(c.b, s + n - 32, 16);
      std::memcpy(d + 16, b.b, 16);
      std::memcpy(d + n - 32, c.b, 16);
    }
    std::memcpy(d, a.b, 16);
    std::memcpy(d + n - 16, e.b, 16);
  } else if (n >= 8) {
    uint64_t a, e;
    std::memcpy(&a, s, 8);
    std::memcpy(&e, s + n - 8, 8);
    std::memcpy(d, &a, 8);
    std::memcpy(d + n - 8, &e, 8);
  } else {
    uint32_t a;
    std::memcpy(&a, s, 4);
    std::memcpy(d, &a, 4);
  }
}

// The physical address Direct3D writes in a packet for a guest address (rlwinm 12,20,31; addi 512; rlwinm 0,19,19;
// clrlwi 3; add): the 0xE0000000 range is 4 KB further.
constexpr uint32_t Physical(uint32_t a) {
  return (Rlwinm(Rlwinm(a, 12, 20, 31) + 512u, 0, 19, 19)) + (a & 0x1FFFFFFFu);
}

// ---------------------------------------------------------------------------------------------------------------------
// State and statistics. Nothing here is written on the usual path of a call.
enum : uint32_t { kIndexed = 0, kVertices = 1, kEntries = 2 };
inline constexpr const char* kNames[kEntries] = {"DrawIndexedVertices", "DrawVertices"};
inline constexpr uint32_t kSlots[kEntries] = {60, 61};  // PPCContext::host_counters (the hot hooks use 0..59)
inline constexpr uint32_t kCountdownBits = 20, kCountdownMask = (1u << kCountdownBits) - 1;
inline constexpr uint32_t kAbTarget = 19;  // banjo_ab_target (16 deferred recording, 17 and 18 EDRAM items)

enum : uint32_t {
  kDeclineCount = 0,   // more than 65535 vertices
  kDeclineWindow,      // the command buffer window is not plainly usable
  kDeclineBuffer,      // no index buffer
  kDeclineSpace,       // late: the draw packets need space (the original finishes the call)
  kDeclineExtents,     // late: the tile-extents list needs a new block
  kDeclineDevice,      // a device object outside the game heap
  kDeclines
};
inline constexpr const char* kDeclineNames[kDeclines] = {"more than 65535 vertices", "command buffer window",
                                                         "no index buffer", "draw packets need space (late)",
                                                         "extents block (late)", "device outside the game heap"};

struct Entry {
  std::atomic<uint8_t> state{0};  // 0 not read yet, 1 off, 2 on
  bool verify = false;
  uint32_t guard_calls = 64, period = 4096;
  std::atomic<uint64_t> declined[kDeclines]{};
  std::atomic<uint64_t> helper_blocks{0};  // blocks handed to Direct3D's own writer (did not fit)
  std::atomic<uint64_t> checked{0}, compared{0}, forced{0}, not_comparable_declined{0},
      raced{0}, differences{0};
  std::atomic<uint64_t> timed_ns{0}, timed_calls{0}, uses{0};
  std::atomic<uint64_t> original_ns{0}, original_calls{0};  // the in-run A/B's "off" halves (a measurement)
};
inline Entry g_entry[kEntries];
inline std::atomic<int> g_logged{0};

inline void Bump(std::atomic<uint64_t>& c, uint64_t n = 1) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

inline uint8_t Init(uint32_t which) {
  Entry& e = g_entry[which];
  const bool own = which == kIndexed ? REXCVAR_GET(banjo_d3d_native_draw_indexed)
                                     : REXCVAR_GET(banjo_d3d_native_draw_vertices);
  e.verify = REXCVAR_GET(banjo_cpu_verify);
  e.guard_calls = uint32_t(std::clamp(REXCVAR_GET(banjo_hot_guard_calls), 0, 4095));
  uint32_t period = 1;
  while (period < uint32_t(std::max(1, REXCVAR_GET(banjo_hot_guard_period))) && period < (1u << kCountdownBits))
    period <<= 1;
  e.period = period;
  const uint8_t state = ::banjo::EnhancementActive(::banjo::EnhancementGroup::kCpu, own) ? 2 : 1;
  e.state.store(state, std::memory_order_relaxed);
  return state;
}

// ---------------------------------------------------------------------------------------------------------------------
// The run writers (one inlined loop each; pointer cursors and locals only).

// The command buffer window [w, limit + 96) is plainly usable by the inline writers: sane, a multiple of four, one
// stretch of host memory, the soft limit inside it, and clear of the device structure (whose shadows are read and
// whose pointer words are stored while it is written). Everything the native code writes lies in [w + 4, limit + 8),
// or, for the draw packets, at most 64 bytes past the soft limit.
[[gnu::always_inline]] inline bool WindowOk(uint32_t d, uint32_t w, uint32_t limit, uint32_t soft) {
  return w < limit && soft <= limit && limit <= 0xFFFFFF00u && (w & 3u) == 0 && w >= 0x10000u &&
         PhysOff(w) == PhysOff(limit + 95u) && (limit + 96u <= d || w >= d + dev::kSize);
}

// sub_821FC1C8 (r3 = device, r4 = dirty bits with bit 63 first, r5 = first register, r6 = shadow): one type-0 packet
// per run of set bits, 16 registers (four vectors, 64 bytes) a bit, padded in front with type-2 packets so that the
// vectors land 16-aligned. Returns the new write pointer (the last word written). The caller has checked that every
// run passes the original's own test (w + 4 * (16 * blocks + 3) < limit): w + 80 * bits + 12 < limit.
[[gnu::always_inline]] inline uint32_t ConstantRuns(uint8_t* base, uint32_t w, uint64_t m, uint32_t reg0,
                                                    const uint8_t* shadow) {
  uint32_t bit = 0;
  do {
    const uint32_t zeros = uint32_t(std::countl_zero(m));
    m <<= zeros;
    bit += zeros;
    const uint32_t blocks = ~m ? uint32_t(std::countl_zero(~m)) : 64u;
    uint8_t* p = Raw(base, w + 4u);
    const uint32_t pads = ((8u - w) & 15u) >> 2;  // w & 15: 4 -> 1, 0 -> 2, 12 -> 3, 8 -> 0
    for (uint32_t k = 0; k < pads; ++k) {
      S32(p, 0x80000000u);
      p += 4;
    }
    S32(p, (((blocks * 16u - 1u) << 16) & 0xFFFF0000u) | (reg0 + bit * 16u));
    p += 4;
    const uint8_t* s = shadow + size_t(bit) * 64u;
    const uint8_t* const end = s + size_t(blocks) * 64u;
    do {
      Copy16(p, s);
      Copy16(p + 16, s + 16);
      Copy16(p + 32, s + 32);
      Copy16(p + 48, s + 48);
      p += 64;
      s += 64;
    } while (s != end);
    w += 4u * pads + 4u + 64u * blocks;
    bit += blocks;
    m = blocks == 64u ? 0 : m << blocks;
  } while (m);
  return w;
}

// sub_821FBC70 (same arguments): one type-0 packet per run of set bits, one register a bit. The caller has checked
// that every run passes the original's test (w + 4 * length < limit): w + 8 * bits < limit.
[[gnu::always_inline]] inline uint32_t RegisterRuns(uint8_t* base, uint32_t w, uint64_t m, uint32_t reg0,
                                                    const uint8_t* shadow) {
  uint32_t bit = 0;
  do {
    const uint32_t zeros = uint32_t(std::countl_zero(m));
    m <<= zeros;
    bit += zeros;
    const uint32_t length = ~m ? uint32_t(std::countl_zero(~m)) : 64u;
    uint8_t* p = Raw(base, w + 4u);
    S32(p, (((length - 1u) << 16) & 0xFFFF0000u) | (reg0 + bit));
    p += 4;
    const uint8_t* const s = shadow + size_t(bit) * 4u;  // words, guest order to guest order
    if (length <= 16u) [[likely]] {
      CopyWordsSmall(p, s, length * 4u);
    } else {
      std::memcpy(p, s, size_t(length) * 4u);
    }
    w += 4u + 4u * length;
    bit += length;
    m = length == 64u ? 0 : m << length;
  } while (m);
  return w;
}

// sub_821F9BB8 (r3 = device, r4 = dirty bits): the fetch constants, six registers (24 bytes) a bit from register
// 0x4800, the values 8-aligned (a type-2 packet is stored after the write pointer and kept when the pointer is 4 mod
// 8), then the end packet its fast path writes: register 0x5000 x 3 = (0, 0x00025000, 0). The caller has checked that
// every run passes the original's test ((6 * length + 5) * 4 + w < limit): w + 32 * bits + 20 < limit.
[[gnu::always_inline]] inline uint32_t FetchRuns(uint8_t* base, uint32_t w, uint64_t m, const uint8_t* shadow) {
  uint32_t bit = 0;
  do {
    const uint32_t zeros = uint32_t(std::countl_zero(m));
    m <<= zeros;
    bit += zeros;
    const uint32_t length = ~m ? uint32_t(std::countl_zero(~m)) : 64u;
    uint8_t* p = Raw(base, w + 4u);
    S32(p, 0x80000000u);
    const uint32_t skip = w & 7u;  // 0 or 4
    p += skip;
    w += skip;
    S32(p, (((length * 6u - 1u) << 16) & 0xFFFF0000u) | (0x4800u + bit * 6u));
    p += 4;
    const uint8_t* s = shadow + size_t(bit) * 24u;
    const uint8_t* const end = s + size_t(length) * 24u;
    do {
      uint64_t a, b, c;
      std::memcpy(&a, s, 8);
      std::memcpy(&b, s + 8, 8);
      std::memcpy(&c, s + 16, 8);
      std::memcpy(p, &a, 8);
      std::memcpy(p + 8, &b, 8);
      std::memcpy(p + 16, &c, 8);
      p += 24;
      s += 24;
    } while (s != end);
    w += 4u + 24u * length;
    bit += length;
    m = length == 64u ? 0 : m << length;
  } while (m);
  uint8_t* const p = Raw(base, w + 4u);
  S32(p, 0x00025000u);
  S32(p + 4, 0);
  S32(p + 8, 0x00025000u);
  S32(p + 12, 0);
  return w + 16u;
}

// banjo_d3d_records_native: the operations of the packets ConstantRuns, RegisterRuns and FetchRuns write (the same
// runs; fillers have none). The caller has checked kBlockRoom.
[[gnu::always_inline]] inline uint32_t* ConstantRunsOps(uint32_t* o, uint64_t m, uint32_t reg0, const uint8_t* shadow) {
  uint32_t bit = 0;
  do {
    const uint32_t zeros = uint32_t(std::countl_zero(m));
    m <<= zeros;
    bit += zeros;
    const uint32_t blocks = ~m ? uint32_t(std::countl_zero(~m)) : 64u;
    o = ::banjo::records::EmitRun(o, reg0 + bit * 16u, shadow + size_t(bit) * 64u, blocks * 16u);
    bit += blocks;
    m = blocks == 64u ? 0 : m << blocks;
  } while (m);
  return o;
}
[[gnu::always_inline]] inline uint32_t* RegisterRunsOps(uint32_t* o, uint64_t m, uint32_t reg0, const uint8_t* shadow) {
  uint32_t bit = 0;
  do {
    const uint32_t zeros = uint32_t(std::countl_zero(m));
    m <<= zeros;
    bit += zeros;
    const uint32_t length = ~m ? uint32_t(std::countl_zero(~m)) : 64u;
    o = ::banjo::records::EmitRun(o, reg0 + bit, shadow + size_t(bit) * 4u, length);
    bit += length;
    m = length == 64u ? 0 : m << length;
  } while (m);
  return o;
}
[[gnu::always_inline]] inline uint32_t* FetchRunsOps(uint32_t* o, uint64_t m, const uint8_t* shadow) {
  uint32_t bit = 0;
  do {
    const uint32_t zeros = uint32_t(std::countl_zero(m));
    m <<= zeros;
    bit += zeros;
    const uint32_t length = ~m ? uint32_t(std::countl_zero(~m)) : 64u;
    o = ::banjo::records::EmitRun(o, 0x4800u + bit * 6u, shadow + size_t(bit) * 24u, length * 6u);
    bit += length;
    m = length == 64u ? 0 : m << length;
  } while (m);
  // The end packet: register 0x5000 x 3 = (0, 0x00025000, 0).
  o[0] = ::banjo::records::kRun | (3u << 8);
  o[1] = 0x5000u;
  o[2] = 0;
  o[3] = 0x00025000u;
  o[4] = 0;
  return o + 5;
}

// banjo_d3d_records_native: what one of Direct3D's own helpers wrote, [from, to] (the words after the write pointer
// it was given, to the one it left), as operations. Null when they cannot be (a barrier, another record's marker, a
// partial packet, no room, or a packet that may not start where it is in the region from `first`: MayStartAt).
[[gnu::noinline]] inline uint32_t* HelperOps(uint32_t* o, uint32_t* end, uint8_t* base, uint32_t first, uint32_t from,
                                             uint32_t to) {
  using namespace ::banjo::records;
  if (to < from || to - from > 0x20000u) return nullptr;
  const uint32_t n = (to - from) / 4;
  if (!n) return o;
  if (uint64_t(end - o) < 2ull * n + 8u) return nullptr;
  const uint8_t* const src = Raw(base, from + 4u);
  const uint32_t physical = Physical(from + 4u);
  for (uint32_t i = 0; i < n;) {
    uint32_t size = 0;
    const Kind kind = Classify(src, i, n, size);
    if (kind == Kind::kBarrier || kind == Kind::kPartial || kind == Kind::kOurs) return nullptr;
    if (!MayStartAt((from + 4u + i * 4u - first) / 4u, kind)) return nullptr;
    o = Emit(src, physical, i, size, kind, o);
    i += size;
  }
  return o;
}

// ---------------------------------------------------------------------------------------------------------------------
// The draw functions. `indexed`: D3DDevice_DrawIndexedVertices(device, type, base vertex, start index, count), else
// D3DDevice_DrawVertices(device, type, start, count). `force_helpers` (the check's stress form): every block goes
// through Direct3D's own writer. Returns false when the original must run (or finish) the call. `ops`
// (banjo_d3d_records_native): where the record's operations go; its `cur` is null after the call when they could not
// all be written (always after a false return).
template <bool indexed>
[[gnu::noinline]] inline bool Draw(PPCContext& ctx, uint8_t* base, bool force_helpers, Entry& e,
                                   ::banjo::records::RecordOps* ops = nullptr) {
  const uint64_t d64 = ctx.r3.u64;
  const uint32_t d = uint32_t(d64);
  if (d64 < kDeviceLow || d64 >= kDeviceHigh) [[unlikely]] return false;
  uint8_t* const dp = base + d;
  // The arguments (the helpers below use the same registers).
  const uint32_t type = ctx.r4.u32;
  const uint32_t first = ctx.r5.u32;  // the base vertex (indexed) / the first vertex
  const uint32_t start = ctx.r6.u32;  // indexed: the first index
  const uint32_t count = indexed ? ctx.r7.u32 : ctx.r6.u32;
  // The argument registers as they came: the helpers called below change r3-r12, and when the original finishes the
  // call it must get its own arguments (the first PC runs of this code crashed there: an original started with the
  // registers a helper had left).
  const uint64_t arg3 = ctx.r3.u64, arg4 = ctx.r4.u64, arg5 = ctx.r5.u64, arg6 = ctx.r6.u64, arg7 = ctx.r7.u64;
  const auto restore_arguments = [&]() __attribute__((always_inline)) {
    ctx.r3.u64 = arg3;
    ctx.r4.u64 = arg4;
    ctx.r5.u64 = arg5;
    ctx.r6.u64 = arg6;
    ctx.r7.u64 = arg7;
  };
  if (ops) ops->cur = nullptr;  // (set again at the end when every operation was written)
  if (count > 65535u) [[unlikely]] {
    Bump(e.declined[kDeclineCount]);
    return false;
  }
  uint32_t w = L32(dp + dev::kWptr), limit = L32(dp + dev::kLimit);
  if (!WindowOk(d, w, limit, L32(dp + dev::kSoftLimit))) [[unlikely]] {
    Bump(e.declined[kDeclineWindow]);
    return false;
  }
  const uint32_t ib = indexed ? L32(dp + dev::kIndexBuffer) : 0x40000000u;
  if (ib < 0x10000u || ib > 0xFFFFFF00u || PhysOff(ib) != PhysOff(ib + 27u)) [[unlikely]] {
    Bump(e.declined[kDeclineBuffer]);
    return false;
  }
  // banjo_d3d_records_native: the operation cursor (a local pointer: a store through it cannot alias it) and the
  // call's first word (the marker's three words are from there).
  uint32_t* o = ops ? ops->begin : nullptr;
  uint32_t* const o_end = ops ? ops->end : nullptr;
  const uint32_t first_word = w + 4u, limit0 = limit;
  // After a helper: its packets as operations (from the write pointer it was given).
  const auto helper_ops = [&](uint32_t from) __attribute__((always_inline)) {
    if (!o) return;
    o = limit == limit0 ? HelperOps(o, o_end, base, first_word, from, w) : nullptr;
  };
  // The original's frame (stwu r1,-208(r1) / -160): the back chain word, and its helpers run below it.
  const uint64_t r1 = ctx.r1.u64;
  const uint32_t frame = uint32_t(r1) - (indexed ? 208u : 160u);
#if !defined(REX_BACK_CHAIN_ELIDE)
  S32(Raw(base, frame), uint32_t(r1));
#endif
  ctx.r1.u32 = frame;

  // FlushState, as both draw functions inline it. All five masks are loaded first: later blocks see these values even
  // if a helper changes the device.
  const uint64_t vs = L64(dp + dev::kDirtyVsConsts);
  const uint64_t misc = L64(dp + dev::kDirtyMisc);
  const uint64_t fetch = L64(dp + dev::kDirtyFetch);
  uint64_t groups = L64(dp + dev::kDirtyGroups);
  const uint64_t ps = L64(dp + dev::kDirtyPsConsts);
  bool valid = true;          // the window is usable
  bool ok = !force_helpers;  // ... and the inline writers may be used
  // After a recompiled helper: the command buffer may be another segment.
  const auto reload = [&]() __attribute__((always_inline)) {
    w = L32(dp + dev::kWptr);
    limit = L32(dp + dev::kLimit);
    valid = WindowOk(d, w, limit, L32(dp + dev::kSoftLimit));
    ok = valid && !force_helpers;
  };
  // A block of register words: sub_821FBC70(device, mask, first register, shadow), at most `max_bits` bits.
  const auto registers = [&](uint64_t mask, uint32_t reg, uint32_t shadow, uint32_t max_bits) __attribute__((always_inline)) {
    if (ok && uint64_t(w) + 8u * max_bits < limit) [[likely]] {
      w = RegisterRuns(base, w, mask, reg, dp + shadow);
      S32(dp + dev::kWptr, w);
      if (o) o = o_end - o >= ptrdiff_t(::banjo::records::kBlockRoom) ? RegisterRunsOps(o, mask, reg, dp + shadow) : nullptr;
    } else {
      Bump(e.helper_blocks);
      const uint32_t from = w;
      ctx.r6.u64 = d64 + shadow;
      ctx.r5.s64 = int64_t(reg);
      ctx.r4.u64 = mask;
      ctx.r3.u64 = d64;
      sub_821FBC70(ctx, base);
      reload();
      helper_ops(from);
    }
  };
  const auto constants = [&](uint64_t mask, uint32_t reg, uint32_t shadow) __attribute__((always_inline)) {
    if (ok && uint64_t(w) + 80u * uint32_t(std::popcount(mask)) + 12u < limit) [[likely]] {
      w = ConstantRuns(base, w, mask, reg, dp + shadow);
      S32(dp + dev::kWptr, w);
      if (o) o = o_end - o >= ptrdiff_t(::banjo::records::kBlockRoom) ? ConstantRunsOps(o, mask, reg, dp + shadow) : nullptr;
    } else {
      Bump(e.helper_blocks);
      const uint32_t from = w;
      ctx.r6.u64 = d64 + shadow;
      ctx.r5.s64 = int64_t(reg);
      ctx.r4.u64 = mask;
      ctx.r3.u64 = d64;
      sub_821FC1C8(ctx, base);
      reload();
      helper_ops(from);
    }
  };
  if (vs) {
    constants(vs, 0x4000, dev::kShadowVsConsts);
    S64(dp + dev::kDirtyVsConsts, 0);
  }
  if (ps) {
    constants(ps, 0x4400, dev::kShadowPsConsts);
    S64(dp + dev::kDirtyPsConsts, 0);
  }
  if (groups) {
    if (Rlwinm(uint32_t(groups), 0, 11, 14)) {  // the draw-time shader load (Direct3D's own code)
      const uint32_t from = w;
      ctx.r4.u64 = groups;
      ctx.r3.u64 = d64;
      sub_821FAF10(ctx, base);
      groups = ctx.r3.u64;
      reload();
      helper_ops(from);
    }
    if (L64(dp + dev::kDirtyRb) & groups) {  // the re-send block (Direct3D's own code)
      const uint32_t from = w;
      ctx.r4.u64 = groups;
      ctx.r5.u64 = L32(dp + dev::kReg2203);
      ctx.r3.u64 = d64;
      __imp__sub_82239418(ctx, base);
      groups = ctx.r3.u64;
      reload();
      helper_ops(from);
    }
    if (groups & 0xFFFull) registers(Rldicr(groups, 52, 11), 0x2200, dev::kShadow2200, 12);
    if (Rlwinm(uint32_t(groups), 0, 15, 19)) registers(Rldicr(groups, 47, 4), 0x2180, dev::kShadow2180, 5);
    if (groups & Rldicr(0xFFFF, 42, 21)) registers(Rldicr(groups, 6, 15), 0x2000, dev::kShadow2000, 16);
    if (groups & 0x000003FFFFE00000ull) registers(Rldicr(groups, 22, 20), 0x2100, dev::kShadow2100, 21);
    S64(dp + dev::kDirtyGroups, 0);
  }
  if (fetch) {
    if (fetch & 0xFFFFFFFFull) {
      const uint64_t mask = Rldicr(fetch, 32, 31);
      if (ok && uint64_t(w) + 32u * uint32_t(std::popcount(mask)) + 20u < limit) [[likely]] {
        w = FetchRuns(base, w, mask, dp + dev::kShadowFetch);
        S32(dp + dev::kWptr, w);
        if (o) o = o_end - o >= ptrdiff_t(::banjo::records::kBlockRoom) ? FetchRunsOps(o, mask, dp + dev::kShadowFetch) : nullptr;
      } else {
        Bump(e.helper_blocks);
        const uint32_t from = w;
        ctx.r4.u64 = mask;
        ctx.r3.u64 = d64;
        sub_821F9BB8(ctx, base);
        reload();
        helper_ops(from);
      }
    }
    if (fetch & Rldicr(0x1FFFFF, 34, 29)) registers(Rldicr(fetch, 9, 20), 0x2280, dev::kShadow2280, 21);
    S64(dp + dev::kDirtyFetch, 0);
  }
  if (misc) {
    if (misc & 0x3FFFFFFFFFull) registers(Rldicr(misc, 26, 37), 0x2300, dev::kShadow2300, 38);
    if (misc & Rldicr(255, 38, 25)) registers(Rldicr(misc, 18, 7), 0x2380, dev::kShadow2380, 8);
    if (misc & Rldicr(63, 49, 14)) {  // the 0x2388 block (Direct3D's own code)
      const uint32_t from = w;
      ctx.r4.u64 = Rldicr(misc, 9, 5);
      ctx.r3.u64 = d64;
      __imp__sub_821DD898(ctx, base);
      reload();
      helper_ops(from);
    }
    if (misc & Rldicr(1, 56, 63)) registers(0xFFFFFFFFFF000000ull, 0x4900, dev::kShadowBoolLoop, 40);
    S64(dp + dev::kDirtyMisc, 0);
  }
  ctx.r1.u64 = r1;  // (the original's epilogue; nothing below needs the frame)

  // The draw. From here on the original would do the same from its first instruction (every mask is zero now), so
  // what cannot be done here is left to it, before anything more is changed.
  const uint32_t soft = L32(dp + dev::kSoftLimit);
  if (!valid || w > soft) [[unlikely]] {
    Bump(e.declined[kDeclineSpace]);
    restore_arguments();
    return false;
  }
  const bool tiling = (dp[dev::kFlags] & 1u) != 0;
  uint32_t entry = 0;
  if (tiling) {
    entry = L32(dp + dev::kExtentsNext);
    if (entry + 16u > L32(dp + dev::kExtentsEnd) || entry < 0x10000u || entry > 0xFFFFFF00u ||
        PhysOff(entry) != PhysOff(entry + 3u) || (entry + 4u > w && entry < limit + 96u)) [[unlikely]] {
      Bump(e.declined[kDeclineExtents]);
      restore_arguments();
      return false;
    }
  }
  uint8_t* p = Raw(base, w + 4u);
  S32(p, 0x00002102u);  // VGT_INDX_OFFSET
  S32(p + 4, first);
  p += 8;
  const uint32_t packet = w + 8u;  // the word before the draw's packets (the extents entry names it)
  // The draw packet's first word (Direct3D's per-draw viz/initiator extension).
  const uint32_t viz_index = dp[dev::kVizIndex];
  uint32_t viz = dp[dev::kVizTable + viz_index];
  viz = Rlwimi(viz, viz_index != 0 ? 1u : 0u, 8, 23, 23) & 319u;
  uint32_t words;
  uint32_t header, w1, w2 = 0, w3 = 0;
  if (indexed) {
    const uint8_t* const ibp = Raw(base, ib);
    const uint32_t data = L32(ibp + 24), common = L32(ibp);
    const uint32_t endian = Rlwinm(common, 1, 0, 1);
    w1 = Rlwinm(count, 16, 0, 15) | (type & 0x3Fu);
    w2 = Physical(Rlwinm(start, 1, 0, 30) + data);
    w3 = (count & 0x00FFFFFFu) | endian;
    if (common & 0x80000000u) {  // 32-bit indices
      w3 = Rlwinm(count, 1, 8, 30) | endian;
      w2 = Physical(Rlwinm(start, 2, 0, 29) + data);
      w1 |= 2048u;
    }
    header = 0xC0032201u;
    words = 5;
  } else {
    w1 = Rlwinm(count, 16, 0, 15) | (type & 0x3Fu) | 128u;
    header = 0xC0012201u;
    words = 3;
  }
  if (o) {  // banjo_d3d_records_native: VGT_INDX_OFFSET, then the draw packet (and the tile wrapper around it)
    if (o_end - o < 64) {
      o = nullptr;
    } else {
      o[0] = ::banjo::records::kRun | (1u << 8);
      o[1] = 0x2102u;
      o[2] = first;
      o += 3;
      const uint32_t host[4] = {viz, w1, w2, w3};
      if (!tiling) {
        o = ::banjo::records::EmitType3(o, header, Physical(packet + 4u), host, words - 1u);
      } else {
        const uint32_t address = entry + 4u;
        const uint32_t ext[2] = {26u, (Rlwinm(Rlwinm(address, 12, 20, 31) + 512u, 0, 19, 19) + Rlwinm(address, 0, 3, 30)) | 1u};
        const uint32_t event[1] = {25u};
        o = ::banjo::records::EmitRef(o, 0xC0006000u, Physical(packet + 4u));
        o = ::banjo::records::EmitType3(o, header, Physical(packet + 12u), host, words - 1u);
        o = ::banjo::records::EmitRef(o, 0xC0006000u, Physical(packet + 12u + 4u * words));
        o = ::banjo::records::EmitType3(o, 0xC0015A01u, Physical(packet + 20u + 4u * words), ext, 2);
        o = ::banjo::records::EmitType3(o, 0xC0004601u, Physical(packet + 32u + 4u * words), event, 1);
      }
    }
  }
  if (!tiling) [[likely]] {
    S32(p, header);
    S32(p + 4, viz);
    S32(p + 8, w1);
    if (indexed) {
      S32(p + 12, w2);
      S32(p + 16, w3);
    }
    w += 8u + 4u * words;
  } else {
    // Predicated tiling: the draw between a tile-mask placeholder (Direct3D fills it in from the draw's screen
    // extents, which the GPU writes into the list entry) and the packets that ask for those extents.
    uint8_t* const ep = Raw(base, entry);
    S32(ep, packet);
    S32(dp + dev::kExtentsNext, entry + 16u);
    S32(p, 0xC0006000u);  // SET_BIN_MASK_LO
    S32(p + 4, 0xFFFFFFFFu);
    S32(p + 8, header);
    S32(p + 12, viz);
    S32(p + 16, w1);
    p += 20;
    if (indexed) {
      S32(p, w2);
      S32(p + 4, w3);
      p += 8;
    }
    const uint32_t address = entry + 4u;
    S32(p, 0xC0006000u);
    S32(p + 4, 0x80000000u);
    S32(p + 8, 0xC0015A01u);  // EVENT_WRITE_EXT, predicated
    S32(p + 12, 26);
    S32(p + 16, (Rlwinm(Rlwinm(address, 12, 20, 31) + 512u, 0, 19, 19) + Rlwinm(address, 0, 3, 30)) | 1u);
    S32(p + 20, 0xC0004601u);  // EVENT_WRITE, predicated
    S32(p + 24, 25);
    w += 8u + 4u * (words + 9u);
  }
  S32(dp + dev::kWptr, w);
  if (ops) ops->cur = o;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The check (see the top of the file): the native code and the original, each on a private copy of the device with
// the same scratch command buffer, compared byte for byte; then the original for real.
inline constexpr uint32_t kScratchBuffer = 0x40000;  // the copies' command buffer (bytes)
struct Scratch {
  uint32_t copy = 0;    // guest address of the device copy; the buffer follows at copy + 0x6000
  uint32_t checks = 0;  // this thread's checked calls (every 8th forces Direct3D's own writers)
  std::vector<uint8_t> before;   // the device as both runs get it
  std::vector<uint8_t> device;   // the copy after the native run
  std::vector<uint8_t> packets;  // the packets of the native run
};
inline thread_local Scratch* t_scratch = nullptr;  // checked calls only

struct Region {
  uint32_t a, n;
  std::vector<uint8_t> before, native;
};

inline void AddRegion(std::vector<Region>& r, uint32_t a, uint32_t n) {
  // (Heap objects, physical allocations and the image's data: nothing of these is below 0x40000000.)
  if (a >= 0x40000000u && a <= 0xFFFF0000u && n && n <= 0x10000u && PhysOff(a) == PhysOff(a + n - 1u)) r.push_back({a, n, {}, {}});
}

// The guest memory outside the device a draw call may write: the bound vertex shader's binding states and program
// descriptors, the bindings' microcode (Direct3D patches it for the vertex declaration), the declaration's id and
// Direct3D's id counter, the tile-extents entry.
inline std::vector<Region> ExternalRegions(uint8_t* base, uint32_t d, uint32_t& vs_out) {
  std::vector<Region> r;
  const uint8_t* const dp = base + d;
  const uint32_t vs = L32(dp + dev::kVertexShader), decl = L32(dp + dev::kVertexDecl);
  vs_out = 0;
  if (vs >= 0x40000000u && vs <= 0xFFFF0000u && PhysOff(vs) == PhysOff(vs + 0x3FFu)) {
    vs_out = vs;
    AddRegion(r, vs, 0x400);
    // Binding 0, and binding 1 when the shader has one (flag 0x20 of the word at +872).
    const uint32_t bindings = (L32(Raw(base, vs + 872u)) & 0x20u) ? 2u : 1u;
    for (uint32_t i = 0; i < bindings; ++i) {
      const uint32_t prog = vs + L32(Raw(base, vs + (i + 112u) * 8u));
      if (prog < 0x40000000u || prog > 0xFFFF0000u || PhysOff(prog) != PhysOff(prog + 880u)) continue;
      AddRegion(r, prog + 872u, 8);
      AddRegion(r, L32(Raw(base, prog + 872u)) + L32(Raw(base, vs + 32u)), L32(Raw(base, prog + 876u)));
    }
  }
  if (decl) AddRegion(r, decl + 48u, 4);
  AddRegion(r, kGlobalDeclIds, 4);
  if (dp[dev::kFlags] & 1u) AddRegion(r, L32(dp + dev::kExtentsNext), 16);
  return r;
}

inline void Difference(Entry& e, uint32_t which, const std::string& detail) {
  ::me::native::VerifyDifference();
  Bump(e.differences);
  e.state.store(1, std::memory_order_relaxed);
  if (g_logged.fetch_add(1, std::memory_order_relaxed) < 16)
    REXLOG_ERROR("[d3d native] DIFFERENCE {}: {}; this entry point is off from now on (the original produces every "
                 "checked call's result)", kNames[which], detail);
}

template <bool indexed>
[[gnu::noinline]] inline bool Checked(PPCContext& ctx, uint8_t* base, uint32_t& counter) {
  const uint32_t which = indexed ? kIndexed : kVertices;
  Entry& e = g_entry[which];
  // The next checked call of this thread (the counter word: banjo_hot_guest.h).
  const uint32_t phase = counter >> kCountdownBits;
  if (e.verify) {
    counter = 0;
  } else if (phase < e.guard_calls) {
    counter = (phase + 1) << kCountdownBits;
  } else {
    counter = (phase << kCountdownBits) | (e.period - 1);
    e.uses.fetch_add(e.period, std::memory_order_relaxed);
  }
  PPCFunc* const original = indexed ? &__imp__sub_8263F358 : &__imp__sub_821FB880;
  const uint32_t d = ctx.r3.u32;
  if (ctx.r3.u64 < kDeviceLow || ctx.r3.u64 >= kDeviceHigh) {
    original(ctx, base);
    return true;
  }
  Scratch* t = t_scratch;
  if (!t) {
    t = new Scratch();
    t->copy = rex::system::kernel_state()->memory()->SystemHeapAlloc(0x6000 + kScratchBuffer, 0x1000);
    t->before.resize(dev::kSize);
    t->device.resize(dev::kSize);
    t_scratch = t;
    REXLOG_INFO("[d3d native] a thread's check area: device copy at {:08X}, its command buffer at {:08X}", t->copy,
                t->copy + 0x6000u);
  }
  if (t->copy < kDeviceLow || t->copy >= kDeviceHigh - 0x6000u - kScratchBuffer) {  // no scratch: not checked
    original(ctx, base);
    return true;
  }
  Bump(e.checked);
  uint8_t* const dp = base + d;
  uint8_t* const cp = base + t->copy;
  const uint32_t buffer = t->copy + 0x6000u;
  const uint32_t cw0 = buffer + 64u + (L32(dp + dev::kWptr) & 15u);  // the real write pointer's alignment phase
  const uint32_t fence_at = L32(dp + dev::kFenceCompleted);
  const bool fence_ok = fence_at >= 0x40000000u && fence_at <= 0xFFFF0000u;
  const uint32_t fence0 = fence_ok ? L32(Raw(base, fence_at)) : 0;
  uint32_t vs = 0;
  std::vector<Region> regions = ExternalRegions(base, d, vs);
  for (Region& x : regions) {
    x.before.resize(x.n);
    std::memcpy(x.before.data(), Raw(base, x.a), x.n);
  }
  // The copy, as both runs get it.
  std::memcpy(t->before.data(), dp, dev::kSize);
  S32(t->before.data() + dev::kWptr, cw0);
  S32(t->before.data() + dev::kLimit, buffer + kScratchBuffer - 0x1000u);
  S32(t->before.data() + dev::kSoftLimit, buffer + kScratchBuffer - 0x1400u);
  const bool force = (++t->checks & 7u) == 0;
  // 1. The native code on the copy.
  std::memcpy(cp, t->before.data(), dev::kSize);
  bool done;
  {
    PPCContext c = ctx;  // checked calls only: a copy's run gets a context of its own
    c.r3.u64 = t->copy;
    done = Draw<indexed>(c, base, force, e);
  }
  const uint32_t cw1 = L32(cp + dev::kWptr);
  if (done && (cw1 < cw0 || cw1 - cw0 > kScratchBuffer)) done = false;
  if (done) {
    std::memcpy(t->device.data(), cp, dev::kSize);
    t->packets.assign(Raw(base, cw0 + 4u), Raw(base, cw0 + 4u) + (cw1 - cw0));
  }
  for (Region& x : regions) {
    x.native.resize(x.n);
    std::memcpy(x.native.data(), Raw(base, x.a), x.n);
    std::memcpy(Raw(base, x.a), x.before.data(), x.n);
  }
  if (!done) {  // declined (more than 65535 vertices, a new extents block, ...): not compared
    original(ctx, base);
    Bump(e.not_comparable_declined);
    return true;
  }
  // 2. The original on the copy, from the same bytes. It takes no path the native run did not take: no call for
  //    space (the scratch buffer's limits are far), no new extents block (the native run would have declined).
  std::memcpy(cp, t->before.data(), dev::kSize);
  {
    PPCContext c = ctx;
    c.r3.u64 = t->copy;
    original(c, base);
  }
  const uint32_t ow1 = L32(cp + dev::kWptr);
  const uint32_t fence1 = fence_ok ? L32(Raw(base, fence_at)) : 0;
  std::string detail;
  bool fence_words_only = false;
  if (ow1 != cw1) {
    detail = fmt::format("{} bytes of packets, the original {}", cw1 - cw0, ow1 - cw0);
  } else if (std::memcmp(Raw(base, cw0 + 4u), t->packets.data(), cw1 - cw0) != 0) {
    uint32_t k = 0;
    while (L32(Raw(base, cw0 + 4u + k)) == L32(t->packets.data() + k)) k += 4;
    detail = fmt::format("packet word {} of {}: native {:08X}, the original {:08X}", k / 4, (cw1 - cw0) / 4,
                         L32(t->packets.data() + k), L32(Raw(base, cw0 + 4u + k)));
  }
  if (detail.empty() && std::memcmp(cp, t->device.data(), dev::kSize) != 0) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < dev::kSize && n < 12; ++i) {
      if (cp[i] == t->device[i]) continue;
      detail += fmt::format("{}device+{}: native {:02X}, the original {:02X} (before {:02X})", n ? "; " : "", i,
                            t->device[i], cp[i], t->before[i]);
      ++n;
    }
  }
  if (detail.empty()) {
    for (const Region& x : regions) {
      // Compared: what the native code itself writes outside the device (the tile-extents entry). The shader state
      // regions are written only by Direct3D's own shader load, which both runs call; other threads load the same
      // shader objects and bump the same id counter between the two runs (a forced-form DrawVertices check on the
      // main device saw the counter one higher in one run), so they are captured and put back, not compared.
      if (!(t->before[dev::kFlags] & 1u) || x.a != L32(t->before.data() + dev::kExtentsNext)) continue;
      const uint8_t* now = Raw(base, x.a);
      if (std::memcmp(now, x.native.data(), x.n) == 0) continue;
      uint32_t k = 0;
      while (k < x.n && now[k] == x.native[k]) ++k;
      detail = fmt::format("guest byte {:08X}: native {:02X}, the original {:02X}", x.a + k, x.native[k], now[k]);
      // The bindings' fence words (object + 64 and + 480) are written by every thread that loads this shader (its
      // device's next fence): another thread's store between the two runs is not a difference.
      if (x.a == vs && x.n == 0x400) {
        fence_words_only = true;
        for (uint32_t i = 0; i < x.n; ++i)
          if (now[i] != x.native[i] && !((i >= 64 && i < 68) || (i >= 480 && i < 484))) fence_words_only = false;
      }
      break;
    }
  }
  // Put back what the original's copy run wrote outside the device, then the call for real.
  for (Region& x : regions) std::memcpy(Raw(base, x.a), x.before.data(), x.n);
  original(ctx, base);
  if (!detail.empty() && (fence0 != fence1 || fence_words_only)) {
    // The GPU's completed fence moved between the two runs (the binding check reads it), or another thread loaded the
    // same shader meanwhile.
    Bump(e.raced);
    return true;
  }
  Bump(e.compared);
  if (force) Bump(e.forced);
  if (e.verify) ::me::native::g_verify_native_checks.fetch_add(1, std::memory_order_relaxed);
  if (!detail.empty())
    Difference(e, which, fmt::format("{} (device {:08X}{})", detail, d, force ? ", forced helper form" : ""));
  return true;
}


inline uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}

// One call in 16 is timed (the report's cost line: the console's measure of this code without the profiler).
template <bool indexed>
[[gnu::noinline]] inline bool Timed(PPCContext& ctx, uint8_t* base, Entry& e, ::banjo::records::RecordOps* ops) {
  const uint64_t t0 = NowNs();
  const bool done = Draw<indexed>(ctx, base, false, e, ops);
  if (done) {
    Bump(e.timed_ns, NowNs() - t0);
    Bump(e.timed_calls);
  }
  return done;
}

// banjo_direct_state_ab_s with banjo_ab_target = 19, the "off" half: the original, one call in 16 timed the same way
// (a per-thread tick in the context's slot 62), so one run gives both costs per call under the same load.
template <bool indexed>
[[gnu::noinline]] inline bool OriginalTimed(PPCContext& ctx, uint8_t* base, Entry& e) {
  PPCFunc* const original = indexed ? &__imp__sub_8263F358 : &__imp__sub_821FB880;
  if ((++ctx.host_counters[62] & 15u) != 0) {
    original(ctx, base);
    return true;
  }
  const uint64_t t0 = NowNs();
  original(ctx, base);
  Bump(e.original_ns, NowNs() - t0);
  Bump(e.original_calls);
  return true;
}

}  // namespace native

// The hooks' entry: true when the call is done (natively, or by the original inside a checked call). `ops`
// (banjo_d3d_records_native, may be null): its `cur` is null afterwards unless the native code wrote every operation.
template <bool indexed>
[[gnu::always_inline]] inline bool NativeDraw(PPCContext& ctx, uint8_t* base, ::banjo::records::RecordOps* ops = nullptr) {
  using namespace native;
  if (ops) ops->cur = nullptr;
  Entry& e = g_entry[indexed ? kIndexed : kVertices];
  const uint8_t state = e.state.load(std::memory_order_relaxed);
  if (state != 2) [[unlikely]] {
    if (state == 1 || Init(indexed ? kIndexed : kVertices) != 2) return false;
  }
  // Devices of the game's heap only (the five the census saw are there): a precaution for the check, which copies
  // 24,064 bytes from the device. Never declined in the PC runs.
  if (ctx.r3.u64 - 0x40000000ull >= 0x3F000000ull) [[unlikely]] {
    Bump(e.declined[kDeclineDevice]);
    return false;
  }
  if (!::me::native::AbOn(kAbTarget)) [[unlikely]] return OriginalTimed<indexed>(ctx, base, e);
  uint32_t& counter = ctx.host_counters[kSlots[indexed ? kIndexed : kVertices]];
  if ((counter & kCountdownMask) == 0) [[unlikely]] return Checked<indexed>(ctx, base, counter);
  --counter;
  if ((counter & 15u) == 0) [[unlikely]] return Timed<indexed>(ctx, base, e, ops);
  return Draw<indexed>(ctx, base, false, e, ops);
}

// Every 10 s (from the Swap hook): the counters of each entry point that was reached.
inline void NativeReport() {
  using namespace native;
  static uint64_t last_ns[kEntries] = {}, last_timed[kEntries] = {};
  for (uint32_t i = 0; i < kEntries; ++i) {
    Entry& e = g_entry[i];
    const uint64_t checked = e.checked.load(std::memory_order_relaxed);
    const uint64_t timed = e.timed_calls.load(std::memory_order_relaxed), ns = e.timed_ns.load(std::memory_order_relaxed);
    if (!checked && !timed && !e.original_calls.load(std::memory_order_relaxed)) continue;
    std::string declines;
    for (uint32_t k = 0; k < kDeclines; ++k)
      if (const uint64_t n = e.declined[k].load(std::memory_order_relaxed)) declines += fmt::format(" {} {};", kDeclineNames[k], n);
    const uint64_t dt = timed - last_timed[i], dns = ns - last_ns[i];
    last_timed[i] = timed;
    last_ns[i] = ns;
    const double per_us = dt ? double(dns) / double(dt) / 1000.0 : 0.0;
    REXLOG_INFO("[d3d native] {} {}: about {} native calls so far; checked calls {}: {} compared with the original "
                "(both on a device copy; {} of them in the forced helper form), {} not compared (the native run "
                "declined), {} raced (GPU progress or another thread's load of the same shader between the runs), {} "
                "differences | the original ran instead for:{} | blocks left to Direct3D's own writer: {} | cost "
                "(last 10 s): {:.2f} us per native "
                "call (every 16th timed: {}), about {:.1f} ms in all = {:.2f} % of one core{}",
                kNames[i], e.state.load(std::memory_order_relaxed) == 2 ? "ON" : "off", timed * 16u, checked,
                e.compared.load(std::memory_order_relaxed), e.forced.load(std::memory_order_relaxed),
                e.not_comparable_declined.load(std::memory_order_relaxed), e.raced.load(std::memory_order_relaxed),
                e.differences.load(std::memory_order_relaxed), declines.empty() ? " nothing" : declines,
                e.helper_blocks.load(std::memory_order_relaxed), per_us, dt, per_us * double(dt) * 16.0 / 1000.0,
                per_us * double(dt) * 16.0 / 1000.0 / 100.0,
                e.original_calls.load(std::memory_order_relaxed)
                    ? fmt::format(" | in-run A/B (cumulative): native {:.3f} us per call ({} timed), the original {:.3f} us per "
                                  "call ({} timed)",
                                  timed ? double(ns) / double(timed) / 1000.0 : 0.0, timed,
                                  double(e.original_ns.load(std::memory_order_relaxed)) /
                                      double(e.original_calls.load(std::memory_order_relaxed)) / 1000.0,
                                  e.original_calls.load(std::memory_order_relaxed))
                    : std::string());
  }
}

}  // namespace banjo::d3d
