/**
 * @file    gpu/d3d/hooks/draw.cpp
 * @brief   Draw, clear and resolve entry points, and the shader setters.
 *
 * The draws replace N&B's D3D entirely: the originals would flush the device's
 * dirty state into GPU packets, including binding shaders by reading their
 * microcode, which renderer-owned shader objects do not carry. Clear, Resolve
 * and tiling queue their host work and do the device work of N&B's D3D
 * natively (d3d_native.h): dirty masks, the tiling state, fences on the
 * destination.
 *
 * The host work runs on the render thread (queued.h); a device recording a
 * command buffer records its draws, clears and resolves (command_buffers.h)
 * instead. Neither takes State().mutex here.
 */
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>

#include "renut_engine/gpu/banjo_renut_select.h"
#include <rex/types.h>

#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/guest/guest_records.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/queued.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/backend/video.h"
#include "profiler/profiler.h"  // katamari-nx: the frame boundary at Present
#include <atomic>

using namespace renut::gpu;

// A float argument also takes the GPR (or stack) slot of its position, which
// the marshaling does not skip: the u32 after each double in the hooks'
// parameters stands in for it.

namespace {

constexpr u32 kE_OUTOFMEMORY = 0x8007000Eu;
// D3DVIEWPORT9 {0, 0, 0xFFFF, 0xFFFF, 0.0, 1.0} in N&B's data (unk_82001744):
// the whole surface.
constexpr u32 kFullViewport = 0x820FAEB4;  // katamari-nx: 0x82001744->0x820FAEB4
// D3DRS_HIZWRITEENABLE's index in the render state table.
constexpr u32 kRenderStateHiZWriteEnable = 97;

// Guest memory Begin*Vertices hands out; the game writes one draw's vertices
// into it before its next D3D call. One per device, as devices record on
// different threads.
constexpr u32 kVerticesScratchSize = 4u << 20;
std::mutex g_vertices_scratch_mutex;  // devices call from their own threads
std::unordered_map<u32, u32> g_vertices_scratch;

u32 VerticesScratch(u32 device) {
  // blur-nx (2026-11-17, the 24-hour run): a device's scratch never changes once made (the map is never erased), so
  // the thread's last answers are kept and a Begin*Vertices draw takes the lock only for a device new to its thread.
  thread_local u32 t_device[4], t_scratch[4];
  const u32 slot = (device * 2654435761u) >> 30;
  if (t_device[slot] == device && t_scratch[slot])
    return t_scratch[slot];
  std::lock_guard lock(g_vertices_scratch_mutex);
  u32 &scratch = g_vertices_scratch[device];
  if (!scratch)
    scratch = records::AllocPhysicalBlock(kVerticesScratchSize);
  t_device[slot] = device;
  t_scratch[slot] = scratch;
  return scratch;
}

bool Active() {
  auto &s = State();
  return s.host_ready && !s.shutting_down.load(std::memory_order_acquire);
}

void StoreU32(u32 va, u32 value) {
  if (auto *p = GuestPtr<be_u32>(va))
    *p = value;
}

u32 LoadU32(u32 va) {
  const auto *p = GuestPtr<const be_u32>(va);
  return p ? u32(*p) : 0;
}

bool ReadRect(u32 va, ClearRect &rect) {
  const auto *p = GuestPtr<const be_u32>(va);
  if (!p)
    return false;
  rect = {i32(u32(p[0])), i32(u32(p[1])), i32(u32(p[2])), i32(u32(p[3]))};
  return true;
}

bool ReadVector4(u32 va, float out[4]) {
  const auto *p = GuestPtr<const be_u32>(va);
  if (!p)
    return false;
  for (int i = 0; i < 4; ++i) {
    const u32 bits = p[i];
    std::memcpy(&out[i], &bits, 4);
  }
  return true;
}

// void D3DDevice_DrawVertices(D3DDevice*, D3DPRIMITIVETYPE, UINT StartVertex,
//                             UINT VertexCount) [0x821FB880]
void D3DDevice_DrawVertices_hook(u32 device, u32 primitive, u32 start_vertex, u32 vertex_count) {
  if (!Active())
    return;
  DrawCall call;
  call.primitive = primitive;
  call.start_vertex = start_vertex;
  call.count = vertex_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, 0, 0);
  else
    QueueDraw(device, call, 0, 0);
}

// void D3DDevice_DrawIndexedVertices(D3DDevice*, D3DPRIMITIVETYPE, INT BaseVertexIndex,
//                                    UINT StartIndex, UINT IndexCount) [0x8263F358]
void D3DDevice_DrawIndexedVertices_hook(u32 device, u32 primitive, u32 base_vertex,
                                        u32 start_index, u32 index_count) {
  if (!Active())
    return;
  DrawCall call;
  call.primitive = primitive;
  call.indexed = true;
  call.base_vertex = i32(base_vertex);
  call.start_index = start_index;
  call.count = index_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, 0, 0);
  else
    QueueDraw(device, call, 0, 0);
}

// void D3DDevice_DrawIndexedTessellatedVertices(D3DDevice*, D3DTESSPRIMITIVETYPE,
//     INT BaseVertexIndex, UINT StartIndex, UINT IndexCount) [0x821FF958]
// Replaced like the other draws: the original's state flush binds the shader
// from its microcode, and bugchecks (0xF5) on a tessellation shader without
// the tessellation state the original SetVertexShader keeps. The draw is
// tessellated on the host (DrawCall::tessellation).
void D3DDevice_DrawIndexedTessellatedVertices_hook(u32 device, u32 primitive, u32 base_vertex,
                                                   u32 start_index, u32 index_count) {
  if (!Active())
    return;
  DrawCall call;
  call.primitive = primitive;
  call.tessellation = true;
  call.indexed = true;
  call.base_vertex = i32(base_vertex);
  call.start_index = start_index;
  call.count = index_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, 0, 0);
  else
    QueueDraw(device, call, 0, 0);
}

// void D3DDevice_DrawVerticesUP(D3DDevice*, D3DPRIMITIVETYPE, UINT VertexCount,
//                               const void *pVertexStreamZeroData, UINT Stride) [0x821FD698]
void D3DDevice_DrawVerticesUP_hook(u32 device, u32 primitive, u32 vertex_count, u32 data,
                                   u32 stride) {
  if (!Active() || !data)
    return;
  DrawCall call;
  call.primitive = primitive;
  call.count = vertex_count;
  call.up_vertices = GuestPtr<const u8>(data);
  call.up_stride = stride;
  call.up_vertex_count = vertex_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, vertex_count * stride, 0);
  else
    QueueDraw(device, call, vertex_count * stride, 0);
}

// void D3DDevice_DrawIndexedVerticesUP(D3DDevice*, D3DPRIMITIVETYPE, UINT MinVertexIndex,
//     UINT NumVertices, UINT IndexCount, const void *pIndexData, D3DFORMAT IndexDataFormat,
//     const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) [0x8263F2B8]
void D3DDevice_DrawIndexedVerticesUP_hook(u32 device, u32 primitive, u32 min_vertex,
                                          u32 num_vertices, u32 index_count, u32 index_data,
                                          u32 index_format, u32 vertex_data, u32 stride) {
  if (!Active() || !index_data || !vertex_data)
    return;
  DrawCall call;
  call.primitive = primitive;
  call.indexed = true;
  call.count = index_count;
  call.up_indices = GuestPtr<const u8>(index_data);
  call.up_indices_32 = (index_format & 4) != 0;  // D3DFMT_INDEX32
  call.up_vertices = GuestPtr<const u8>(vertex_data);
  call.up_stride = stride;
  call.up_vertex_count = min_vertex + num_vertices;
  const u32 vertex_bytes = call.up_vertex_count * stride;
  const u32 index_bytes = index_count * (call.up_indices_32 ? 4 : 2);
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, vertex_bytes, index_bytes);
  else
    QueueDraw(device, call, vertex_bytes, index_bytes);
}

// void *D3DDevice_BeginVertices(D3DDevice*, D3DPRIMITIVETYPE, UINT VertexCount,
//                               UINT VertexStreamZeroStride) [0x821FBD30]
// Callers write the vertices, then end inline by restoring m_pRing from the
// device, so the draw's state is captured now and its vertices copied at the
// device's next hook (FlushDeferredVertices).
u32 D3DDevice_BeginVertices_hook(u32 device, u32 primitive, u32 vertex_count, u32 stride) {
  const u64 bytes = u64(vertex_count) * stride;
  if (!bytes || bytes > kVerticesScratchSize)
    return 0;
  // The inline end: m_pRing = [device + kDeviceOffsetVerticesRingEnd].
  StoreU32(device + d3d::kDeviceOffsetVerticesRingEnd,
           LoadU32(device + u32(offsetof(d3d::Device, m_pRing))));

  const u32 scratch = VerticesScratch(device);
  if (!scratch)
    return 0;
  DrawCall call;
  call.primitive = primitive;
  call.count = vertex_count;
  call.up_vertices = GuestPtr<const u8>(scratch);
  call.up_stride = stride;
  call.up_vertex_count = vertex_count;
  // Either way the device's previous Begin*Vertices data leaves the scratch
  // before the game writes this draw's over it.
  if (IsRecordingCommandBuffer(device))
    RecordDeferredDraw(device, call, call.up_vertices, u32(bytes));
  else if (Active())
    QueueDeferredDraw(device, call, call.up_vertices, u32(bytes));
  else
    FlushDeferredVertices(device);
  return scratch;
}

// A clear: the renderer's, recorded or queued, then N&B's device work.
// whole_target: BeginTiling's, of one tile's worth of EDRAM, which on the
// host is the whole tiled target.
void Clear(u32 device, u32 flags, const ClearRect *rect, const float color[4], float z,
           u32 stencil, bool whole_target) {
  const ClearRect *host_rect = whole_target ? nullptr : rect;
  if (IsRecordingCommandBuffer(device))
    RecordClear(device, flags, host_rect, color, z, stencil);
  else if (Active())
    QueueClear(device, flags, host_rect, color, z, stencil);
  const native::Rect native_rect = rect ? native::Rect{rect->x1, rect->y1, rect->x2, rect->y2}
                                        : native::Rect{};
  native::Clear(native::DeviceAt(device), flags, rect ? &native_rect : nullptr);
}

// A resolve: the renderer's copy (and clear), then N&B's device work.
void Resolve(u32 device, u32 flags, const ClearRect *rect, u32 dest_texture, i32 dest_x,
             i32 dest_y, u32 dest_level, u32 dest_slice, u32 color_va, float clear_z,
             u32 clear_stencil, u32 parameters) {
  float color[4] = {0, 0, 0, 0};
  const bool has_color = color_va && ReadVector4(color_va, color);
  if (IsRecordingCommandBuffer(device)) {
    RecordResolve(device, flags, rect, dest_texture, dest_x, dest_y, dest_level, dest_slice,
                  has_color ? color : nullptr, clear_z, clear_stencil);
  } else if (Active()) {
    QueueResolve(device, flags, rect, dest_texture, dest_x, dest_y, dest_level, dest_slice,
                 has_color ? color : nullptr, clear_z, clear_stencil);
  }
  native::Resolve(native::DeviceAt(device), flags, dest_texture, clear_z, clear_stencil,
                  parameters);
}

// void D3DDevice_ClearF(D3DDevice*, DWORD Flags, const D3DRECT *pRect,
//                       const D3DVECTOR4 *pColor, float Z, DWORD Stencil) [0x8224F110]
// Z takes r7's slot, so Stencil arrives in r8.
void D3DDevice_ClearF_hook(u32 device, u32 flags, u32 rect_va, u32 color_va, double z,
                           u32 /*z_slot*/, u32 stencil) {
  ClearRect rect;
  const bool has_rect = rect_va && ReadRect(rect_va, rect);
  float color[4] = {0, 0, 0, 0};  // unk_82001160 without one
  if (color_va)
    ReadVector4(color_va, color);
  Clear(device, flags, has_rect ? &rect : nullptr, color, float(z), stencil, false);
}

// void D3DDevice_Clear(D3DDevice*, DWORD Count, const D3DRECT *pRects, DWORD Flags,
//     D3DCOLOR Color, float Z, DWORD Stencil) [0x8224F238]: ClearF with the
// color as a vector (XMLoadColor: each byte as 1 + b / 2^23, scaled by
// 2^23 / 255 and offset), per rect. Z takes r8's slot.
void D3DDevice_Clear_hook(u32 device, u32 count, u32 rects_va, u32 flags, u32 color_argb,
                          double z, u32 /*z_slot*/, u32 stencil) {
  const float scale = std::bit_cast<float>(0x47008081u);  // aG, unk_82001150
  auto channel = [scale](u32 byte) {
    return std::fmaf(std::bit_cast<float>(0x3F800000u | (byte & 0xFF)), scale, -scale);
  };
  const float color[4] = {channel(color_argb >> 16), channel(color_argb >> 8),
                          channel(color_argb), channel(color_argb >> 24)};
  if (!rects_va) {
    Clear(device, flags, nullptr, color, float(z), stencil, false);
    return;
  }
  for (u32 i = 0; i < count; ++i) {
    ClearRect rect;
    if (ReadRect(rects_va + i * 16, rect))
      Clear(device, flags, &rect, color, float(z), stencil, false);
  }
}

// void D3DDevice_Resolve(D3DDevice*, DWORD Flags, const D3DRECT *pSourceRect,
//     D3DBaseTexture *pDestTexture, const D3DPOINT *pDestPoint, UINT DestLevel,
//     UINT DestSliceOrFace, const D3DVECTOR4 *pClearColor, float ClearZ,
//     DWORD ClearStencil, const D3DRESOLVE_PARAMETERS *pParameters) [0x821FC7A0]
// ClearZ is in f1 and takes the first stack slot; ClearStencil and pParameters
// are in the two after it.
void D3DDevice_Resolve_hook(u32 device, u32 flags, u32 rect_va, u32 dest_texture, u32 point_va,
                            u32 dest_level, u32 dest_slice, u32 color_va, double clear_z,
                            u32 /*z_slot*/, u32 clear_stencil, u32 parameters) {
  ClearRect rect;
  const bool has_rect = rect_va && ReadRect(rect_va, rect);
  i32 dest_x = 0, dest_y = 0;
  if (const auto *p = GuestPtr<const be_u32>(point_va)) {
    dest_x = i32(u32(p[0]));
    dest_y = i32(u32(p[1]));
  }
  Resolve(device, flags, has_rect ? &rect : nullptr, dest_texture, dest_x, dest_y, dest_level,
          dest_slice, color_va, float(clear_z), clear_stencil, parameters);
}

// void D3DDevice_BeginTiling(D3DDevice*, DWORD Count, const D3DRECT *pTileRects,
//     const D3DVECTOR4 *pClearColor) [0x82215510]: TU3's build dropped the
// arguments BeginTiling never used (Flags, ClearZ, ClearStencil).
// The surfaces bound from here to EndTiling are backed by targets covering
// every tile (SetTilingExtentLocked). N&B's BeginTiling clears with Z 0.0 and
// stencil 0, unless the device records a one-pass Z pass (Flags3 0x20); its
// ring segment for the tiles is left out.
// katamari-nx (2026-10-30): XDK 2.0.5632's BeginTiling keeps the full public form, BeginTiling(Flags, Count,
// pTileRects, pClearColor, ClearZ, ClearStencil) (sub_824EFBC8: r4 Flags, r5 Count, r6 rects, r7 colour, f1 Z,
// r9 stencil). It clears with ClearZ / ClearStencil and keeps them at +0x3390 / +0x3394 for EndTiling (stfs f31 /
// stw r24 there), stores Flags at +0x337C, sets Flags3 0x20 (and 0x10 with Flags bit 2) when Flags has bit 1, and
// skips its first clear when Flags has bit 0 or Flags3 has 0x20. KatamariBeginTiling passes those through; N&B's
// update form (no Flags, Z 0.0, stencil 0) is the call with flags 0 and the default clear values.
static void BeginTilingImpl(u32 device, u32 count, u32 rects_va, u32 color_va, u32 flags, float clear_z,
                            u32 clear_stencil);
void D3DDevice_BeginTiling_hook(u32 device, u32 count, u32 rects_va, u32 color_va) {
  BeginTilingImpl(device, count, rects_va, color_va, 0, 0.0f, 0);
}
static void BeginTilingImpl(u32 device, u32 count, u32 rects_va, u32 color_va, u32 flags, float clear_z,
                            u32 clear_stencil) {
  native::Device d = native::DeviceAt(device);
  if (!d)
    return;
  if (flags & 2) {
    d.SetB(native::off::Flags3, d.B(native::off::Flags3) | 0x20 | ((flags & 4) ? 0x10 : 0));
  }
  std::vector<ClearRect> tiles;
  for (u32 i = 0; i < count; ++i) {
    ClearRect rect;
    if (!ReadRect(rects_va + i * 16, rect))
      break;
    tiles.push_back(rect);
  }
  count = u32(tiles.size());
  if (!IsRecordingCommandBuffer(device)) {
    FlushDeferredVertices(device);
    u32 width = 0, height = 0;
    for (const ClearRect &rect : tiles) {
      width = std::max(width, u32(rect.x2));
      height = std::max(height, u32(rect.y2));
    }
    render_thread::Queue([tiles, width, height] {
      SetTilingExtentLocked(width, height);
      SetTilesLocked(tiles.data(), u32(tiles.size()));
    });
  }

  d.SetW(native::off::TileCount, count);
  u32 extent_x = 0, extent_y = 0;
  for (u32 i = 0; i < count; ++i) {
    const ClearRect &rect = tiles[i];
    extent_x = std::max(extent_x, u32(rect.x2));
    extent_y = std::max(extent_y, u32(rect.y2));
    d.SetW(native::off::TileRects + i * 16, u32(rect.x1));
    d.SetW(native::off::TileRects + i * 16 + 4, u32(rect.y1));
    d.SetW(native::off::TileRects + i * 16 + 8, u32(rect.x2));
    d.SetW(native::off::TileRects + i * 16 + 12, u32(rect.y2));
    d.SetW(native::off::TileOrigins + i * 8, u32(rect.x1) & ~31u);
    d.SetW(native::off::TileOrigins + i * 8 + 4, u32(rect.y1) & ~31u);
  }
  d.SetW(native::off::TilingExtent, extent_x);
  d.SetW(native::off::TilingExtent + 4, extent_y);
  d.SetW(0x337C, flags);  // katamari-nx: 0x338C->0x337C; XDK 2.0.5632 keeps Flags there (N&B's update: 0)

  if (!(flags & 1) && !(d.B(native::off::Flags3) & 0x20)) {
    // The largest tile, from its origin: its clear covers a tile of EDRAM.
    u32 width = 0, height = 0;
    for (u32 i = 0; i < count; ++i) {
      width = std::max(width, u32(tiles[i].x2) - d.W(native::off::TileOrigins + i * 8));
      height = std::max(height, u32(tiles[i].y2) - d.W(native::off::TileOrigins + i * 8 + 4));
    }
    // The viewport (x, y, width, height, min z, max z) and scissor rect, for
    // after the clear.
    float viewport[6];
    for (u32 i = 0; i < 6; ++i)
      viewport[i] = d.F(0x3160 + i * 4);  // katamari-nx: 0x3170->0x3160
    i32 scissor[4];
    for (u32 i = 0; i < 4; ++i)
      scissor[i] = i32(d.W(native::off::ScissorRect + i * 4));
    native::SetViewport(d, kFullViewport);
    native::SetScissorRect(d, 0, 0, 0xFFFF, 0xFFFF);  // dword_82001190
    const ClearRect rect{0, 0, i32(width), i32(height)};
    float color[4] = {0, 0, 0, 0};
    if (color_va)
      ReadVector4(color_va, color);
    Clear(device, 0x3F, &rect, color, clear_z, clear_stencil, true);
    native::SetViewport(d, viewport[0], viewport[1], viewport[2], viewport[3], viewport[4],
                        viewport[5]);
    native::SetScissorRect(d, scissor[0], scissor[1], scissor[2], scissor[3]);
  }
  if (const auto *color = color_va ? GuestPtr<const u8>(color_va) : nullptr)
    std::memcpy(d.bytes + 0x3380, color, 16);  // katamari-nx: 0x3390->0x3380
  else
    std::memset(d.bytes + 0x3380, 0, 16);  // katamari-nx: 0x3390->0x3380
  d.SetF(0x3390, clear_z);  // flt_82000D18 (katamari-nx: 0x33A0->0x3390; XDK 2.0.5632: the ClearZ argument)
  d.SetW(0x3394, clear_stencil);  // katamari-nx: 0x33A4->0x3394; XDK 2.0.5632: the ClearStencil argument

  const u32 target = d.W(native::off::RenderTargets);
  d.SetW(0x31AC, target ? target : d.W(native::off::DepthStencil));  // katamari-nx: 0x31BC->0x31AC
  for (u32 i = 0; i < 5; ++i)  // the 4 targets, then depth
    d.SetW(native::off::TilingSurfaces + i * 4, d.W(native::off::RenderTargets + i * 4));
  d.SetW(0x31A8, 2);  // katamari-nx: 0x31B8->0x31A8
  d.SetB(native::off::Flags, d.B(native::off::Flags) | 0x20);
  d.SetW(native::off::PredicationSet, 0);
  native::UpdateTilePredicated(d);
  d.SetW(native::off::RunPredicationState, 0);
  d.SetW(native::off::Predication, ~0u);
  d.SetW(native::off::TilePredication, 0);
  d.SetW(native::off::RunPredication, 0);
  // The tiles' commands go into a ring segment of their own from a KickOff.
  native::KickOff(device);
  native::SetViewport(d, kFullViewport);
  native::ResetAllState(d);
}

// HRESULT D3DDevice_EndTiling(D3DDevice*, D3DBaseTexture *pDestTexture,
//     const D3DVECTOR4 *pClearColor) [0x821B17C0]: TU3's build dropped the
// arguments EndTiling never used (ResolveFlags, pResolveRects, ClearZ,
// ClearStencil, pParameters).
// N&B's EndTiling resolves each tile's rect (flags 0x300: color and depth
// cleared) with BeginTiling's clear values, the last one with pClearColor,
// Z 1.0 and stencil 0. The host resolve reads each rect from the full-size
// target. Left out: the tiles' ring segment, D3D's WorkerDpc callback that
// replays it per tile, and its fence's packets.
// katamari-nx (2026-10-30): XDK 2.0.5632's EndTiling keeps the full form, EndTiling(ResolveFlags, pResolveRects,
// pDestTexture, pClearColor, ClearZ, ClearStencil, pParameters) (sub_824F0150: r4, r5, r6, r7, f1, r9, r10). Every
// tile but the last resolves with ResolveFlags | 0x100 when a colour target is bound (+0x3090) | 0x200 when a depth
// surface is (+0x30A0), BeginTiling's clear values (+0x3380 colour, +0x3390 Z, +0x3394 stencil) and no parameters;
// the last with ResolveFlags, the caller's colour, Z, stencil and parameters. The dest point is each tile's origin
// in both. (N&B's update dropped these arguments: always 0x300, then Z 1.0, stencil 0.) The game passes no
// pResolveRects (all three calls at 0x8272F2E0..0x8272F384): a non-null one is logged and the tiles used.
static u32 EndTilingImpl(u32 device, bool katamari, u32 resolve_flags, u32 rects_va, u32 dest_texture, u32 color_va,
                         float clear_z, u32 clear_stencil, u32 params);
u32 D3DDevice_EndTiling_hook(u32 device, u32 dest_texture, u32 color_va) {
  return EndTilingImpl(device, false, 0x300, 0, dest_texture, color_va, 1.0f, 0, 0);
}
static u32 EndTilingImpl(u32 device, bool katamari, u32 resolve_flags, u32 rects_va, u32 dest_texture, u32 color_va,
                         float clear_z, u32 clear_stencil, u32 params) {
  native::Device d = native::DeviceAt(device);
  if (!d)
    return 0;
  if (rects_va) {
    static bool once = false;
    if (!once) {
      once = true;
      RGPU_WARN("[katamari] EndTiling with pResolveRects 0x{:08X}: resolving the tiles' rects instead", rects_va);
    }
  }
  const u32 count = d.W(native::off::TileCount);
  if (dest_texture) {
    for (u32 i = 0; i < count; ++i) {
      const u32 tile = native::off::TileRects + i * 16;
      ClearRect rect{i32(d.W(tile)), i32(d.W(tile + 4)), i32(d.W(tile + 8)), i32(d.W(tile + 12))};
      // A tile at the surface's edge resolves to a multiple of 8.
      const u32 surface = d.W(0x31AC);  // katamari-nx: 0x31BC->0x31AC
      const auto *s = surface ? GuestPtr<const be_u32>(surface) : nullptr;
      const u32 size = s ? u32(s[9]) : 0;
      if (u32(rect.x2) == (size >> 18) + 1)
        rect.x2 = i32((u32(rect.x2) + 7) & ~7u);
      if (u32(rect.y2) == native::Extract(size, 3, 15) + 1)
        rect.y2 = i32((u32(rect.y2) + 7) & ~7u);
      native::SetPredication(d, 3u << (i * 2));
      native::SetRenderState(d, kRenderStateHiZWriteEnable,
                             (d.B(native::off::Flags3) & 0x20) ? 0 : 2);
      const i32 dest_x = i32(d.W(tile)), dest_y = i32(d.W(tile + 4));
      if (i + 1 < count) {
        u32 flags = resolve_flags;
        if (katamari) {
          if (d.W(native::off::RenderTargets))
            flags |= 0x100;
          if (d.W(native::off::DepthStencil))
            flags |= 0x200;
        }
        Resolve(device, flags, &rect, dest_texture, dest_x, dest_y, 0, 0, device + 0x3380,  // katamari-nx: 0x3390->0x3380
                d.F(0x3390), d.W(0x3394), 0);  // katamari-nx: 0x33A0->0x3390, 0x33A4->0x3394
      } else {
        Resolve(device, resolve_flags, &rect, dest_texture, dest_x, dest_y, 0, 0, color_va,
                clear_z /*N&B: flt_82000C1C*/, clear_stencil, params);
      }
    }
  }
  native::SetPredication(d, 0);
  native::SetRenderState(d, kRenderStateHiZWriteEnable, 2);
  if (count != 1) {
    // Around the WorkerDpc callback.
    native::SetPredication(d, 0x80000000u);
    native::SetPredication(d, 0);
  }
  native::KickOff(device);

  d.SetB(native::off::Flags3, d.B(native::off::Flags3) & 0xCF);
  d.SetB(native::off::Flags, d.B(native::off::Flags) & 0xDF);
  d.SetW(0x31AC, 0);  // katamari-nx: 0x31BC->0x31AC
  d.SetW(native::off::PredicationSet, 0);
  native::UpdateTilePredicated(d);
  d.SetW(native::off::RunPredicationState, 0);
  d.SetW(native::off::Predication, ~0u);
  d.SetW(native::off::TilePredication, 0);
  d.SetW(native::off::RunPredication, 0);
  // sub_821DF678: the segment's fence, unless the device's command memory ran
  // out (EndCommandBuffer marks a device so until the next Begin).
  const bool out_of_memory = (d.B(native::off::Flags1) & 0x20) != 0;
  if (out_of_memory) {
    d.SetB(native::off::Flags1, d.B(native::off::Flags1) & 0xDF);
    native::RewindRing(d);
  } else {
    native::SetFence(device);
  }
  native::SetViewport(d, kFullViewport);

  if (!IsRecordingCommandBuffer(device)) {
    render_thread::Queue([] {
      SetTilingExtentLocked(0, 0);
      SetTilesLocked(nullptr, 0);
    });
  }
  if (out_of_memory) {
    native::ResetAllState(d);
    return kE_OUTOFMEMORY;
  }
  return 0;
}

// void D3DDevice_BeginExport(D3DDevice*, DWORD Index, D3DResource*, DWORD Flags)
// [0x82237FE8]. The renderer finds a draw's memexport streams in its shaders.
void D3DDevice_BeginExport_hook(u32 device, u32 index, u32 resource, u32 flags) {
  native::BeginExport(native::DeviceAt(device), index, resource, flags);
}

// void D3DDevice_EndExport(D3DDevice*, DWORD Index, D3DResource*, DWORD Flags)
// [0x82237D80]: the resource is busy until the current fence, which completes
// once the render thread has written the stream's results.
void D3DDevice_EndExport_hook(u32 device, u32 index, u32 resource, u32 flags) {
  native::EndExport(native::DeviceAt(device), index, resource, flags);
}

// void D3DDevice_Present(D3DDevice*) [0x826352B8]: the automatic back buffer
// resolved into the automatic front buffer, then Swap. N&B creates its device
// without an automatic front buffer, so its callers (XUI, the threaded movie
// player) have nothing to resolve to.
void D3DDevice_Present_hook(u32 device) {
  native::Device d = native::DeviceAt(device);
  if (!d)
    return;
  FlushDeferredVertices(device);
  float viewport[6];
  for (u32 i = 0; i < 6; ++i)
    viewport[i] = d.F(0x3160 + i * 4);  // katamari-nx: 0x3170->0x3160
  i32 scissor[4];
  for (u32 i = 0; i < 4; ++i)
    scissor[i] = i32(d.W(native::off::ScissorRect + i * 4));
  native::SetRenderTarget(d, 0, d.W(native::off::AutoBackBuffer));
  // SynchronizeToPresentationInterval (0x82258778): its swap callback and wait
  // go into the ring; the swap is pending.
  native::SetPredication(d, 0);
  d.SetB(native::off::Flags1, d.B(native::off::Flags1) | d3d::kSwapFlagPending);
  const u32 front_buffer = d.W(native::off::AutoFrontBuffer);
  if (front_buffer)
    Resolve(device, 0, nullptr, front_buffer, 0, 0, 0, 0, 0, 1.0f, 0, 0);
  native::KickOff(device);
  native::SetViewport(d, viewport[0], viewport[1], viewport[2], viewport[3], viewport[4],
                      viewport[5]);
  native::SetScissorRect(d, scissor[0], scissor[1], scissor[2], scissor[3]);
  native::Swap(device, front_buffer, device + 0x3530);  // katamari-nx: 0x3548->0x3530
  // katamari-nx: one game frame ends at Present (N&B's ended at its appMainDraw hook, renut_engine/FPS.cpp): frame
  // time, the Switch profiler's frame marks, the stall watchdog. blur-nx (2026-11-17): g_banjo_game_frames is counted
  // in the Swap hook (device.cpp) that native::Swap above runs: Blur swaps without Present.
  banjo::profiler::FrameBoundary();
}

// void D3DDevice_SetVertexShader(D3DDevice*, D3DVertexShader*) [0x821F90D8]
void D3DDevice_SetVertexShader_hook(u32 device, u32 shader) {
  native::SetVertexShader(native::DeviceAt(device), shader);
}

// void D3DDevice_SetPixelShader(D3DDevice*, D3DPixelShader*) [0x821F58A8]
void D3DDevice_SetPixelShader_hook(u32 device, u32 shader) {
  native::SetPixelShader(native::DeviceAt(device), shader);
}

} // namespace

BANJO_RENUT_HOOK(sub_824D9D08, D3DDevice_DrawVertices_hook);  // D3DDevice_DrawVertices  // katamari-nx: N&B sub_821FB880, signature 0.90 (next 0.77)
BANJO_RENUT_HOOK(sub_824DA0F0, D3DDevice_DrawIndexedVertices_hook);  // D3DDevice_DrawIndexedVertices  // katamari-nx: N&B sub_8263F358, signature 0.87 (next 0.78)
BANJO_RENUT_HOOK(sub_824DA9C0, D3DDevice_DrawIndexedTessellatedVertices_hook);  // D3DDevice_DrawIndexedTessellatedVertices  // katamari-nx: N&B sub_821FF958, signature 0.86 (next 0.65)
BANJO_RENUT_HOOK(sub_824D9758, D3DDevice_DrawVerticesUP_hook);  // D3DDevice_DrawVerticesUP  // katamari-nx: N&B sub_821FD698, D3DDevice_DrawVerticesUP: BeginVertices-style reserve, reads VerticesRingEnd +13428 (N&B +13444) and the flags +10940; sig 0.69/0.55
BANJO_RENUT_UNHOOKED_HOOK(sub_8263F2B8, D3DDevice_DrawIndexedVerticesUP_hook);  // D3DDevice_DrawIndexedVerticesUP  // katamari-nx: not hooked (D3DDevice_DrawIndexedVerticesUP: no counterpart found (sig 0.32))
BANJO_RENUT_HOOK(sub_824D92B8, D3DDevice_BeginVertices_hook);  // D3DDevice_BeginVertices  // katamari-nx: N&B sub_821FBD30, signature 0.87 (next 0.70)
BANJO_RENUT_HOOK(sub_824F3248, D3DDevice_ClearF_hook);  // D3DDevice_ClearF  // katamari-nx: N&B sub_8224F110, D3DDevice_ClearF: sig 0.71/0.40; reads the targets +12432.. and tiling surfaces +12720.. as N&B +12448 / +12736
BANJO_RENUT_HOOK(sub_824F3370, D3DDevice_Clear_hook);  // D3DDevice_Clear  // katamari-nx: N&B sub_8224F238, signature 0.84 (next 0.36)
BANJO_RENUT_HOOK(sub_824F12B8, D3DDevice_Resolve_hook);  // D3DDevice_Resolve  // katamari-nx: N&B sub_821FC7A0, signature 0.78 (next 0.33)
BANJO_RENUT_UNHOOKED_HOOK_SHARED(sub_82246798, D3DDevice_BeginTiling_hook);  // D3DDevice_BeginTiling: N&B's update form, see below
BANJO_RENUT_UNHOOKED_HOOK(sub_821E17C0, D3DDevice_EndTiling_hook);  // D3DDevice_EndTiling: N&B's update form, see below
// katamari-nx (2026-10-30): XDK 2.0.5632's BeginTiling (sub_824EFBC8: it stores the count at TileCount +0x31C4 and
// the targets at TilingSurfaces +0x31B0, where the zones put N&B's fields; the game calls it at 0x8271E62C) and
// EndTiling (sub_824F0150: sig 0.66/0.36, resolves through Resolve sub_824F12B8; called at 0x8272F2E0..0x8272F384),
// with their full arguments read from the registers (the float argument takes r8's place: ClearStencil is r9).
BANJO_RENUT_HOOK_RAW(sub_824EFBC8) {
  BeginTilingImpl(ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r4.u32, float(ctx.f1.f64), ctx.r9.u32);
}
BANJO_RENUT_HOOK_RAW(sub_824F0150) {
  ctx.r3.u64 = EndTilingImpl(ctx.r3.u32, true, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, float(ctx.f1.f64),
                             ctx.r9.u32, ctx.r10.u32);
}
BANJO_RENUT_UNHOOKED_HOOK_SHARED(sub_82237FE8, D3DDevice_BeginExport_hook);  // D3DDevice_BeginExport  // katamari-nx: not hooked (D3DDevice_BeginExport: XDK 2.0.5632 has no memexport functions (as feedingfrenzy2-nx found for 6534))
BANJO_RENUT_UNHOOKED_HOOK_SHARED(sub_82237D80, D3DDevice_EndExport_hook);  // D3DDevice_EndExport  // katamari-nx: not hooked (D3DDevice_EndExport: as above)
BANJO_RENUT_HOOK(sub_824E9428, D3DDevice_Present_hook);  // D3DDevice_Present  // katamari-nx: N&B sub_826352B8, D3DDevice_Present: SynchronizeToPresentationInterval sub_824E89E8, Resolve sub_824F12B8, KickOff sub_824E0298, then Swap sub_824E8E90; sig 0.43/0.26
BANJO_RENUT_HOOK(sub_824D4930, D3DDevice_SetVertexShader_hook);  // D3DDevice_SetVertexShader  // katamari-nx: N&B sub_821F90D8, signature 0.87 (next 0.75)
BANJO_RENUT_HOOK(sub_824D4630, D3DDevice_SetPixelShader_hook);  // D3DDevice_SetPixelShader  // katamari-nx: N&B sub_821F58A8, signature 0.85 (next 0.73)
