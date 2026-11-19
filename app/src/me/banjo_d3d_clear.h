// banjo-nx: Direct3D's Clear as an operation the renderer understands (2026-10-14). The first piece of the API-level
// layer of docs/d3d-native.md that changes what the renderer does, not only what the game threads cost.
//
// What it fixes. Direct3D clears a surface with a rectangle list of strips. The renderer takes every EDRAM tile a strip
// covers whole without a transfer, but the surface's last tile column (and last tile row) is covered only in part: a
// 1024-wide shadow map has a pitch of 1040, so each tile of its right column holds 64 surface pixels and 16 pixels of
// pitch padding. To stay exact the renderer copies those padding pixels in from whichever view owned the tile before
// (another shadow cascade, a colour target at the same EDRAM address), with the stencil passes when the old owner had
// stencil. On the console's level that was about 140 transfers a frame and 23 ms of GPU time a frame at stock clocks
// (renut_003 of 2026-10-14: every top transfer pair had Direct3D's clear shaders as its consumer).
//
// What the layer adds. A hook on Direct3D's one-rectangle clear (sub_821F2750; its only caller is D3DDevice_Clear's
// per-rectangle step sub_8224F110) writes a marker NOP into the command buffer before Direct3D's own packets: the
// rectangle, the sizes of the bound colour target 0 and depth surface (from the surface objects, as sub_8224F110 reads
// them), and the flags; a second marker after them. The ring (me_native_system.cpp) then knows that the rectangle list
// between the markers is a Clear of the whole surface, and widens its overwrite rectangles over the surface's own pitch
// padding and the rest of its last tile row: those tiles are taken whole.
//
// The padding is then whatever the view's image held: no surface reads it (a surface's draws and resolves stay inside
// its width and height), and a surface that shares the EDRAM clears its own area before it uses it. That is the one
// assumption, and banjo_diag_clear_padding = 1 tests it: the padding is filled with a loud value (magenta, depth 0.37)
// after each widened clear, so a read would show on screen (compare halves with banjo_ab_target = 20).
//
// Key banjo_d3d_api_clear (non-CPU, on by default); in-run A/B target 20 switches the ring's use of the markers (the
// markers are written either way).
#pragma once
#include <atomic>
#include <cstdint>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include "cpu/banjo_enhancements.h"
#include "me/banjo_d3d_native.h"

REXCVAR_DEFINE_BOOL(banjo_d3d_api_clear, true, "Banjo/Enhancements",
                    "Direct3D's Clear as an operation (GPU enhancement): a marker before and after Direct3D's clear "
                    "packets tells the renderer that the rectangles are a Clear of the whole surface, so the surface's "
                    "partly covered edge tiles (pitch padding, the rest of the last tile row) are taken whole instead of "
                    "being copied in from the EDRAM's previous owner. In-run A/B target 20")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_d3d_api_resolve, true, "Banjo/Enhancements",
                    "Direct3D's Resolve as an operation (GPU enhancement, step 2 of the API layer): a marker around "
                    "Direct3D's resolve packets gives the renderer the resolved surface's size when the resolve covers "
                    "it whole, so the clear after the resolve takes the surface's edge tiles whole (pitch padding, the "
                    "rest of the last tile row) like the Clear operation, instead of copying them in from their previous "
                    "owner. In-run A/B target 21")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace banjo::d3d::clear {

inline constexpr uint32_t kBegin = 0x424A434Cu;  // 'BJCL': [kBegin, x1, y1, x2, y2, rt0 w | h << 16, depth w | h << 16, flags]
inline constexpr uint32_t kEnd = 0x424A4345u;    // 'BJCE': [kEnd]
inline constexpr uint32_t kBeginWords = 8;
inline constexpr uint32_t kDeviceRt0 = 12440, kDeviceDepth = 12456;  // bound colour target 0 and depth surface

struct Stats {
  std::atomic<uint64_t> calls{0}, marked{0}, no_room{0}, end_no_room{0}, declined{0};
};
inline Stats g_stats;

inline bool On() {
  static const bool on =
      banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_d3d_api_clear));
  return on;
}

// A surface's width | height << 16, read as sub_8224F110 reads it (the word at +36: width - 1 in bits 18-31,
// height - 1 in bits 3-17); 0 without a surface.
inline uint32_t SurfaceSize(uint8_t* base, uint32_t surface) {
  if (surface < 0x10000u || surface > 0xFFFFFF00u) return 0;
  const uint32_t d = native::L32(native::Raw(base, surface + 36u));
  return (((d >> 18) & 0x3FFFu) + 1u) | ((((d >> 3) & 0x7FFFu) + 1u) << 16);
}

// A type-3 NOP with `n` payload words at the device's write pointer, when the segment has room before its soft limit
// (the same window test as the native draw path). Nothing is written otherwise.
inline bool WriteNop(uint8_t* base, uint32_t device, const uint32_t* payload, uint32_t n) {
  uint8_t* const dp = base + device;
  const uint32_t w = native::L32(dp + native::dev::kWptr), limit = native::L32(dp + native::dev::kLimit),
                 soft = native::L32(dp + native::dev::kSoftLimit);
  if (!native::WindowOk(device, w, limit, soft) || w + 4u * (n + 1u) > soft) return false;
  uint8_t* const p = native::Raw(base, w + 4u);
  native::S32(p, 0xC0001000u | ((n - 1u) << 16));
  for (uint32_t i = 0; i < n; ++i) native::S32(p + 4u + 4u * i, payload[i]);
  native::S32(dp + native::dev::kWptr, w + 4u * (n + 1u));
  return true;
}

// sub_821F2750 (r3 = device, r4 = flags, r5 = the rectangle x1, y1, x2, y2): the begin marker. True when it was
// written (the end marker is then due after the original).
inline bool Begin(PPCContext& ctx, uint8_t* base) {
  if (!On()) return false;
  g_stats.calls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t device = ctx.r3.u32, rect = ctx.r5.u32;
  // Devices of the game's heap (as the native draw path), a rectangle in guest memory.
  if (ctx.r3.u64 - 0x40000000ull >= 0x3F000000ull || rect < 0x10000u || rect > 0xFFFFFF00u) {
    g_stats.declined.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint8_t* const rp = native::Raw(base, rect);
  const uint32_t payload[kBeginWords] = {
      kBegin, native::L32(rp), native::L32(rp + 4), native::L32(rp + 8), native::L32(rp + 12),
      SurfaceSize(base, native::L32(base + device + kDeviceRt0)),
      SurfaceSize(base, native::L32(base + device + kDeviceDepth)), ctx.r4.u32};
  if (!WriteNop(base, device, payload, kBeginWords)) {
    g_stats.no_room.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  g_stats.marked.fetch_add(1, std::memory_order_relaxed);
  return true;
}

inline void End(uint32_t device, uint8_t* base) {
  const uint32_t payload[1] = {kEnd};
  if (!WriteNop(base, device, payload, 1)) g_stats.end_no_room.fetch_add(1, std::memory_order_relaxed);
}

// --- Resolve (banjo_d3d_api_resolve, step 2) ---------------------------------------------------------------------
// D3DDevice_Resolve (821FC7A0): r3 = device, r4 = flags (low 3 bits: colour target 0-3, or 4 = the depth surface),
// r5 = the source rectangle (x1, y1, x2, y2) or 0 for the whole surface. The marker: [BJRS, whole surface w | h << 16
// or 0 when the rectangle is not the whole surface]; [BJRE] after Direct3D's packets.
inline constexpr uint32_t kResolveBegin = 0x424A5253u;  // 'BJRS'
inline constexpr uint32_t kResolveEnd = 0x424A5245u;    // 'BJRE'
struct ResolveStats {
  std::atomic<uint64_t> calls{0}, marked{0}, whole{0}, no_room{0};
};
inline ResolveStats g_resolve;

inline bool ResolveOn() {
  static const bool on =
      banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_d3d_api_resolve));
  return on;
}

inline bool ResolveBegin(PPCContext& ctx, uint8_t* base) {
  if (!ResolveOn()) return false;
  g_resolve.calls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t device = ctx.r3.u32, which = ctx.r4.u32 & 7u, rect = ctx.r5.u32;
  if (ctx.r3.u64 - 0x40000000ull >= 0x3F000000ull || which > 4) return false;
  // The surface Direct3D resolves from: colour target `which` (+12440 + 4 * which) or the depth surface (+12456),
  // as sub_821FC7A0 picks it.
  const uint32_t surface = native::L32(base + device + (which == 4 ? kDeviceDepth : kDeviceRt0 + 4u * which));
  const uint32_t size = SurfaceSize(base, surface);
  bool whole = size != 0;
  if (whole && rect) {
    if (rect < 0x10000u || rect > 0xFFFFFF00u) return false;
    const uint8_t* const rp = native::Raw(base, rect);
    whole = native::L32(rp) == 0 && native::L32(rp + 4) == 0 && native::L32(rp + 8) >= (size & 0xFFFFu) &&
            native::L32(rp + 12) >= (size >> 16);
  }
  const uint32_t payload[2] = {kResolveBegin, whole ? size : 0u};
  if (!WriteNop(base, device, payload, 2)) {
    g_resolve.no_room.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  g_resolve.marked.fetch_add(1, std::memory_order_relaxed);
  if (whole) g_resolve.whole.fetch_add(1, std::memory_order_relaxed);
  return true;
}

inline void ResolveEnd(uint32_t device, uint8_t* base) {
  const uint32_t payload[1] = {kResolveEnd};
  WriteNop(base, device, payload, 1);
}

// Every 10 s (from the Swap hook).
inline void Report() {
  static uint64_t last_calls = 0;
  const uint64_t calls = g_stats.calls.load(std::memory_order_relaxed);
  if (calls == last_calls) return;
  last_calls = calls;
  REXLOG_INFO("[d3d clear] banjo_d3d_api_clear (cumulative): {} clears, {} marked, {} without room for the marker, {} "
              "end markers without room, {} declined (device or rectangle outside the game heap)",
              calls, g_stats.marked.load(std::memory_order_relaxed), g_stats.no_room.load(std::memory_order_relaxed),
              g_stats.end_no_room.load(std::memory_order_relaxed), g_stats.declined.load(std::memory_order_relaxed));
  REXLOG_INFO("[d3d resolve] banjo_d3d_api_resolve (cumulative): {} resolves, {} marked, {} of the whole surface, {} "
              "without room for the marker", g_resolve.calls.load(std::memory_order_relaxed),
              g_resolve.marked.load(std::memory_order_relaxed), g_resolve.whole.load(std::memory_order_relaxed),
              g_resolve.no_room.load(std::memory_order_relaxed));
}

}  // namespace banjo::d3d::clear
