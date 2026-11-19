/**
 * @file    gpu/d3d/d3d_native.h
 * @brief   N&B's D3D runtime as native code: the device-state work of its D3D
 *          calls, ported from the game's own code (default.xex), without a GPU.
 *
 * Every D3D call the renderer hooks is implemented natively from N&B's
 * disassembly, here or in the hooks (gpu/d3d/hooks). The ports keep the device's
 * shadow state exactly as N&B's D3D does: the constants, the register images
 * (m_*Packet), the bound objects, the dirty masks (m_Pending) and the fences.
 * The renderer reads that state at every draw. What they leave out is what
 * only the GPU needed: command packets, ring and segment bookkeeping, cache
 * flushes, the GPU-side copies of command buffer resource lists.
 *
 * The command ring is a stub, as in reblue: m_pRing points at a scratch block
 * that KickOff rewinds. N&B code that writes packets inline (D3D's header
 * helpers compiled into the game) writes there, and nothing reads it.
 *
 * Offsets are byte offsets into the device allocation, as N&B's code uses them
 * (d3d.h names the ones the renderer reads).
 */
#pragma once

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

#include <rex/types.h>

#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/guest/guest_memory.h"

namespace renut::gpu::native {

// banjo-nx (2026-10-26, banjo_renut_tags): the device chunks the native layer writes, per recording device. A recorded
// op keeps the device's chunks that changed since the device's previous op (command_buffers.cpp NewOp, its "plain"
// chunks: the fetch constants' twelve of 64 bytes, 0x480-0x77F, bits 0-11, then the tail's from the bool constants,
// 0x2780, to the snapshot's end, bits 12-52), found by comparing every chunk with a shadow: a quarter of each
// recording thread. The setters below mark the chunk of each byte they write in their device's slot, and NewOp
// compares only those. A device without a slot of its own (the title devices, which two threads may use at once, and
// any past kTouchSlots) marks a sink slot nobody reads, and is compared whole. Writes outside the setters mark their chunks with Touch*; a write nothing
// marks (the game's own code writing a device field) is what banjo_renut_tags_check finds.
constexpr u32 kTouchFetchBegin = 0x480, kTouchFetchEnd = 0x780, kTouchTailBegin = 0x2780;
constexpr u32 kTouchLines = (d3d::kDeviceSnapshotSize + 63) / 64;
static_assert(kTouchFetchBegin % 64 == 0 && kTouchFetchEnd % 64 == 0 && kTouchTailBegin % 64 == 0);
struct TouchTable {
  uint64_t bit[kTouchLines];
};
constexpr TouchTable MakeTouchTable() {
  TouchTable t{};
  for (u32 line = 0; line < kTouchLines; ++line) {
    const u32 at = line * 64;
    if (at >= kTouchFetchBegin && at < kTouchFetchEnd)
      t.bit[line] = uint64_t(1) << ((at - kTouchFetchBegin) / 64);
    else if (at >= kTouchTailBegin)
      t.bit[line] = uint64_t(1) << ((kTouchFetchEnd - kTouchFetchBegin) / 64 + (at - kTouchTailBegin) / 64);
  }
  return t;
}
inline constexpr TouchTable kTouchTable = MakeTouchTable();
static_assert((kTouchFetchEnd - kTouchFetchBegin) / 64 + (kTouchLines * 64 - kTouchTailBegin) / 64 <= 64);
// The chunk bit of device byte 'offset'; 0 outside the chunks.
constexpr uint64_t TouchBit(u32 offset) {
  return offset < kTouchLines * 64 ? kTouchTable.bit[offset >> 6] : 0;
}
constexpr uint64_t TouchBits(u32 offset, u32 size) {
  uint64_t bits = 0;
  for (u32 line = offset >> 6; size && line < kTouchLines && line * 64 < offset + size; ++line)
    bits |= kTouchTable.bit[line];
  return bits;
}

// banjo-nx (2026-10-26, banjo_renut_tags, after the first emulator check): the game's own code writes some device
// fields itself (Direct3D's inline setters compiled into it: a ZEnable as the depth control word and the private copy
// at 0x2E64, a sampler's address mode in its fetch constant, the vertex declaration), which no setter of ours marks.
// Such a write sets the field's dirty tag too (m_Pending words 2-4, D3DTAGCOLLECTION), or the GPU would never get the
// new state: the tags pending at an op (and those cleared before it, TouchTags) count as marks. A fetch constant's tag
// (word 3, bit 31 - n) is its chunks, its texture word and the stream words; any other tag the render state's chunks: the
// bool and integer constants and the register packets (0x2780-0x2A7F) and the private render state (0x2E00-0x2EFF; on
// title update 3 0x2E04-0x2F07, its CDevice 4 bytes longer from 0x2AC0 and 8 from 0x2E30).
constexpr uint64_t kStateTagChunks = TouchBits(0x2780, 0x300) | TouchBits(0x2DF4, 0x108);  // katamari-nx: 0x2E00->0x2DF4
struct FetchTagTable {
  uint64_t chunks[32];
};
constexpr FetchTagTable MakeFetchTagTable() {
  FetchTagTable t{};
  for (u32 n = 0; n < 32; ++n) {
    t.chunks[n] = TouchBits(kTouchFetchBegin + n * 24, 24);
    // The texture word of a texture fetch constant (0x3108 + 4n on title update 3; 0x3100 on the base), and the stream
    // words and strides (0x30B4-0x3107; 0x30AC-0x30FF):
    // any fetch constant may hold vertex fetches (SetVertexFetchConstant's slot (0x5F - register) / 3).
    if (n < d3d::kTextureFetchSlots)
      t.chunks[n] |= TouchBits(d3d::kDeviceOffsetTextures + n * 4, 4);
    t.chunks[n] |= TouchBits(d3d::kDeviceOffsetStreamSources, d3d::kDeviceOffsetTextures - d3d::kDeviceOffsetStreamSources);
  }
  return t;
}
inline constexpr FetchTagTable kFetchTagChunks = MakeFetchTagTable();
static_assert(kTouchFetchBegin + 32 * 24 == kTouchFetchEnd);
inline uint64_t TagChunks(uint64_t states, uint64_t fetch, uint64_t misc) {
  uint64_t chunks = (states | misc | (fetch >> 32)) ? kStateTagChunks : 0;
  for (uint32_t bits = uint32_t(fetch); bits; bits &= bits - 1)
    chunks |= kFetchTagChunks.chunks[31 - std::countr_zero(bits)];
  return chunks;
}

// banjo-nx (2026-10-26, setter rewrite, banjo_renut_touch_compare): the setters store a word, half or byte and mark its
// chunk only when the value differs from the device's (Unleashed's compare-on-set): the game sets most of a draw's state
// again unchanged (22 chunks marked an op, 4.9 changed), and every marked chunk is compared and copied by NewOp. Exact:
// an unchanged value leaves the device as it was, and a value changed earlier since the op was marked then. A/B 26.
inline bool g_touch_compare = true;
// banjo-nx (2026-10-26, setter rewrite, banjo_renut_tags_apart): a recording device's setters (a device with a slot of
// its own) keep their dirty tags (m_Pending words 2-4) out of the device: their writes mark their own chunks already, and
// a tag in the device made NewOp compare every chunk the tag may stand for (the 16 render state chunks for any state
// tag) as if the game had written them inline. The device's tags are then the inline writes' alone. Nothing reads a
// recording device's tags but NewOp (the ops keep chunks, not tags); Begin/End/ResetAllState still set and clear them
// whole. Turned off (back to the setters' tags in the device) by the first change a check finds unmarked. A/B 27.
inline bool g_tags_apart = true;

// One slot a cache line (recording threads mark their devices' at once): the devices' 1 to kTouchSlots - 1, then the
// sinks (by the device's address, so two devices' marks meet on one line only by chance).
constexpr u32 kTouchSlots = 16, kTouchSinks = 16;
struct alignas(64) TouchSlot {
  uint64_t chunks = 0;
};
inline TouchSlot g_touch[kTouchSlots + kTouchSinks];
// The devices of slots 1 to kTouchSlots - 1 (0: free).
inline std::atomic<u32> g_touch_va[kTouchSlots] = {};
// Device address -> slot, direct-mapped by a hash of the address (va << 32 | slot; 0 empty), so every hook finds its
// device's slot with one load and no call (a thread_local is a call on the Switch: __aarch64_read_tp). A device takes
// its slot here when it gets one (TrackDeviceTouches, at its creation: before any thread uses it); an entry another
// device's took is found again by TouchSlotSlow.
constexpr u32 kTouchMapBits = 6;
inline std::atomic<uint64_t> g_touch_map[1u << kTouchMapBits] = {};
constexpr u32 TouchMapIndex(u32 va) {
  return (va * 0x9E3779B1u) >> (32 - kTouchMapBits);
}
u32 TouchSlotSlow(u32 va);  // d3d_native_state.cpp
inline u32 TouchSlotOf(u32 va) {
  const uint64_t e = g_touch_map[TouchMapIndex(va)].load(std::memory_order_relaxed);
  if (u32(e >> 32) == va && e)
    return u32(e);
  return TouchSlotSlow(va);
}
// A recording device (CreateDevice, no hardware): gives it a slot, all of its chunks marked. Again for a device made
// again at the same address.
void TrackDeviceTouches(u32 va);  // d3d_native_state.cpp
// The chunks marked since the last take, which clears them; false for a device without a slot.
inline bool TakeTouches(u32 va, uint64_t *chunks) {
  const u32 slot = TouchSlotOf(va);
  if (slot - 1 >= kTouchSlots - 1)
    return false;
  *chunks = g_touch[slot].chunks;
  g_touch[slot].chunks = 0;
  return true;
}

// A guest D3D device's bytes, big-endian guest memory. banjo-nx (2026-10-26): 'slot' fills what was padding, so the
// struct stays 16 bytes (two registers in an AArch64 call; a 24-byte one would go through memory).
struct Device {
  u32 va = 0;
  u32 slot = 0;  // banjo-nx (2026-10-26, banjo_renut_tags): its g_touch slot
  u8 *bytes = nullptr;

  explicit operator bool() const { return bytes != nullptr; }

  // banjo-nx (2026-10-26, banjo_renut_tags): marks the chunks written. For a constant offset the bit is a constant (and
  // nothing for an offset outside the chunks). No test: a device without a slot marks its thread's sink.
  void Touch(u32 offset) const { g_touch[slot].chunks |= TouchBit(offset); }
  void TouchRange(u32 offset, u32 size) const { g_touch[slot].chunks |= TouchBits(offset, size); }
  void TouchAll() const { g_touch[slot].chunks = ~uint64_t(0); }
  // banjo-nx (2026-10-26): before m_Pending words 2-4 are cleared other than by an op (TagChunks).
  void TouchTags() const { g_touch[slot].chunks |= TagChunks(Q(2 * 8), Q(3 * 8), Q(4 * 8)); }

  u32 W(u32 offset) const { return *reinterpret_cast<const be_u32 *>(bytes + offset); }
  void SetW(u32 offset, u32 value) {
    be_u32 &word = *reinterpret_cast<be_u32 *>(bytes + offset);
    if (u32(word) == value && g_touch_compare)  // banjo-nx (2026-10-26): compare-on-set
      return;
    word = value;
    Touch(offset);
  }
  u16 H(u32 offset) const { return *reinterpret_cast<const be_u16 *>(bytes + offset); }
  void SetH(u32 offset, u16 value) {
    be_u16 &half = *reinterpret_cast<be_u16 *>(bytes + offset);
    if (u16(half) == value && g_touch_compare)
      return;
    half = value;
    Touch(offset);
  }
  u8 B(u32 offset) const { return bytes[offset]; }
  void SetB(u32 offset, u8 value) {
    if (bytes[offset] == value && g_touch_compare)
      return;
    bytes[offset] = value;
    Touch(offset);
  }
  float F(u32 offset) const { return std::bit_cast<float>(W(offset)); }
  void SetF(u32 offset, float value) { SetW(offset, std::bit_cast<u32>(value)); }
  u64 Q(u32 offset) const { return *reinterpret_cast<const be_u64 *>(bytes + offset); }
  void SetQ(u32 offset, u64 value) {
    be_u64 &quad = *reinterpret_cast<be_u64 *>(bytes + offset);
    if (u64(quad) == value && g_touch_compare)
      return;
    quad = value;
    Touch(offset);
    Touch(offset + 7);  // a 4-aligned offset may end in the next chunk
  }

  // m_Pending[word] (D3DTAGCOLLECTION).
  u64 Pending(u32 word) const { return Q(word * 8); }
  void SetPending(u32 word, u64 value) { SetQ(word * 8, value); }
  void Dirty(u32 word, u64 bits) {
    if (word >= 2 && g_tags_apart && slot - 1 < kTouchSlots - 1)  // banjo-nx (2026-10-26): banjo_renut_tags_apart
      return;
    SetPending(word, Pending(word) | bits);
  }
};
static_assert(sizeof(Device) == 16);

inline Device DeviceAt(u32 va) {
  return Device{va, TouchSlotOf(va), GuestPtr<u8>(va)};
}

// The PowerPC field operations N&B's code uses, with bits counted from the
// least significant: insrwi (Insert) and extrwi (Extract).
constexpr u32 FieldMask(u32 shift, u32 width) {
  return (width >= 32 ? ~0u : ((1u << width) - 1)) << shift;
}
constexpr u32 Insert(u32 word, u32 value, u32 shift, u32 width) {
  const u32 mask = FieldMask(shift, width);
  return (word & ~mask) | ((value << shift) & mask);
}
constexpr u32 Extract(u32 word, u32 shift, u32 width) {
  return (word >> shift) & FieldMask(0, width);
}

// D3D's GPU address for a CPU address (the expression its code inlines).
constexpr u32 GpuAddress(u32 cpu_address) {
  return (cpu_address & 0x1FFFFFFFu) + (((cpu_address >> 20) + 0x200u) & 0x1000u);
}

// fctiwz / fctidz: truncate, saturating, NaN to the most negative value.
inline i32 TruncateToI32(double value) {
  if (std::isnan(value))
    return std::numeric_limits<i32>::min();
  if (value >= 2147483647.0)
    return std::numeric_limits<i32>::max();
  if (value <= -2147483648.0)
    return std::numeric_limits<i32>::min();
  return static_cast<i32>(value);
}
inline i64 TruncateToI64(double value) {
  if (std::isnan(value))
    return std::numeric_limits<i64>::min();
  if (value >= 9223372036854775807.0)
    return std::numeric_limits<i64>::max();
  if (value <= -9223372036854775808.0)
    return std::numeric_limits<i64>::min();
  return static_cast<i64>(value);
}

// Device fields used by more than one part of the runtime.
namespace off {
constexpr u32 Ring = 0x30;                 // m_pRing
constexpr u32 RingLimit = 0x34;            // m_pRingLimit
constexpr u32 RingGuarantee = 0x38;        // m_pRingGuarantee
constexpr u32 PredicatedPending2 = 0x28;   // m_Predicated_PendingMask2
constexpr u32 SetRenderStateCall = 0x40;   // [101]
constexpr u32 SetSamplerStateCall = 0x1D4; // [20]
constexpr u32 GetRenderStateCall = 0x224;  // [101]
constexpr u32 GetSamplerStateCall = 0x3B8; // [20]
constexpr u32 TextureFetch = 0x480;        // m_Constants.TextureFetch[26], 24 bytes each
constexpr u32 VertexFetch = 0x6F0;         // m_Constants.VertexFetch[18], 8 bytes each
constexpr u32 SurfaceInfo = 0x2880;        // m_DestinationPacket
constexpr u32 Color0Info = 0x2884;
constexpr u32 DepthInfo = 0x2888;
constexpr u32 WindowOffset = 0x28C0;       // m_WindowPacket
constexpr u32 WindowScissorTL = 0x28C4;
constexpr u32 WindowScissorBR = 0x28C8;
constexpr u32 ColorMask = 0x28DC;          // m_ValuesPacket
constexpr u32 DepthControl = 0x2934;       // m_ControlPacket
constexpr u32 BlendControl0 = 0x2938;
constexpr u32 ColorControl = 0x293C;
constexpr u32 HiControl = 0x2940;
constexpr u32 ClipControl = 0x2944;
constexpr u32 ModeControl = 0x2948;
constexpr u32 BlendControl1 = 0x2958;
constexpr u32 BlendControl2 = 0x295C;
constexpr u32 BlendControl3 = 0x2960;
constexpr u32 FenceCompletion = 0x2A90;    // -> the completed-fence word
constexpr u32 FenceTag = 0x2A9C;           // the current fence
constexpr u32 CommandBufferTagMask = 0x2AA0;  // Common bits a recorded buffer tracks
constexpr u32 TilePredication = 0x2AB4;    // SetPredication / SetCommandBufferPredication args
constexpr u32 RunPredication = 0x2AB8;
constexpr u32 Flags = 0x2ABC;              // bytes 0x2ABC-0x2ABF, see below
constexpr u32 Flags1 = 0x2ABD;
constexpr u32 Flags2 = 0x2ABE;
constexpr u32 Flags3 = 0x2ABF;
// TU3: the fields from 0x2E30 to 0x3390 are 8 bytes further on.
constexpr u32 ScissorRect = 0x317C;        // SetScissorRect's RECT (katamari-nx: 0x318C->0x317C)
constexpr u32 IndexBuffer = 0x308C;  // katamari-nx: 0x309C->0x308C
constexpr u32 RenderTargets = 0x3090;      // [4] (katamari-nx: 0x30A0->0x3090)
constexpr u32 DepthStencil = 0x30A0;  // katamari-nx: 0x30B0->0x30A0
constexpr u32 StreamSources = 0x30A4;      // [16] (katamari-nx: 0x30B4->0x30A4)
constexpr u32 StreamStrides = 0x30E8;      // u8[16], stride / 4 (katamari-nx: 0x30F8->0x30E8)
constexpr u32 Textures = 0x30F8;           // [26] (katamari-nx: 0x3108->0x30F8)
constexpr u32 Predication = 0x319C;        // tile predication of the next draws, ~0 none (katamari-nx: 0x31AC->0x319C)
constexpr u32 RunPredicationState = 0x31A0;  // katamari-nx: 0x31B0->0x31A0
constexpr u32 PredicationSet = 0x31A4;     // 1 while SetPredication's mask is non-zero (katamari-nx: 0x31B4->0x31A4)
constexpr u32 TilingSurfaces = 0x31B0;     // the 4 targets and depth BeginTiling bound (katamari-nx: 0x31C0->0x31B0)
constexpr u32 TileCount = 0x31C4;  // katamari-nx: 0x31D4->0x31C4
constexpr u32 TileRects = 0x31C8;          // D3DRECT[TileCount] (katamari-nx: 0x31D8->0x31C8)
constexpr u32 TileOrigins = 0x32B8;        // per tile, x & ~31, y & ~31 (katamari-nx: 0x32C8->0x32B8)
constexpr u32 TilingExtent = 0x3374;       // widest and tallest tile edge (katamari-nx: 0x3384->0x3374)
constexpr u32 CommandBuffer = 0x34B4;      // the D3DCommandBuffer being recorded (katamari-nx: 0x34CC->0x34B4)
constexpr u32 SideBuffer = 0x4140;  // katamari-nx: 0x4158->0x4140
constexpr u32 AutoDepthStencil = 0x39D0;   // InitializePresentationParameters' surfaces (katamari-nx: 0x39E8->0x39D0)
constexpr u32 AutoFrontBuffer = 0x39D4;  // katamari-nx: 0x39EC->0x39D4
constexpr u32 AutoBackBuffer = 0x39D8;  // katamari-nx: 0x39F0->0x39D8
}  // namespace off

// Flags (0x2ABC) bits: 0x01 the draws that follow run in each tile's pass,
// 0x08/0x04 command buffer recording modes, 0x10/0x20 tiling, 0x80 no GPU
// hardware. Flags1 (0x2ABD): 0x02 fences complete as they are written, 0x20
// the device's command memory ran out.
constexpr u8 kFlagsNoHardware = 0x80;
constexpr u8 kFlags1ImmediateFences = 0x02;

// Ring stub and fences (hooks/device.cpp).
void RewindRing(Device device);
// CDevice::KickOff: ends the device's batch of work (and its fence).
u32 KickOff(u32 device_va);
// CDevice::BlockOnFence(Fence, Type, NoKick=FALSE).
void BlockOnFence(u32 device_va, u32 fence);
// The title device D3D internals reach through VdGlobalDevice.
u32 GlobalDevice();
// D3DDevice_Swap (D3DDevice_Present ends with it).
void Swap(u32 device_va, u32 front_buffer, u32 parameters);
// CDevice::SetFence on its own (EndTiling's): the current fence covers the
// work so far, and the device moves on to the next.
void SetFence(u32 device_va);

// Resources (hooks/resource.cpp).
// D3DDevice_CreateSurface(Width, Height, D3DFORMAT, D3DMULTISAMPLE_TYPE,
//     const D3DSURFACE_PARAMETERS*): the new surface, or 0.
u32 CreateSurface(u32 width, u32 height, u32 format, u32 multisample, u32 parameters_va);
// D3DResource_AddRef (a texture level's first reference is one on its texture).
u32 AddRefResource(u32 resource);

// Device state (d3d_native_state.cpp).
void SetRenderState(Device device, u32 state, u32 value);
void SetSamplerState(Device device, u32 sampler, u32 state, u32 value);
// The getters of N&B's state tables (field 0): the value as D3D reports it.
u32 GetRenderState(Device device, u32 state);
u32 GetSamplerState(Device device, u32 sampler, u32 state);
// InitializeApiState's walk of the default render and sampler state tables.
void InitializeStateTables(Device device);
void SetTexture(Device device, u32 sampler, u32 texture);
void SetStreamSource(Device device, u32 stream, u32 vertex_buffer, u32 offset, u32 stride);
void SetIndices(Device device, u32 index_buffer);
void SetVertexDeclaration(Device device, u32 declaration);
void SetVertexShader(Device device, u32 shader);
void SetPixelShader(Device device, u32 shader);
void SetViewport(Device device, float x, float y, float width, float height, float min_z,
                 float max_z);
void SetViewport(Device device, u32 viewport_va);  // D3DVIEWPORT9*
void SetScissorRect(Device device, i32 left, i32 top, i32 right, i32 bottom);
void SetRenderTarget(Device device, u32 index, u32 surface);
void SetDepthStencilSurface(Device device, u32 surface);
void SetSurfaceInfo(Device device, u32 surface);
void SetPredication(Device device, u32 mask);
void SetCommandBufferPredication(Device device, u32 tile_predication, u32 run_predication);
void SetShaderGPRAllocation(Device device, u32 flags, u32 vertex_count, u32 pixel_count);
void CleanupGprAllocationSetup(Device device);
void SetScreenExtentQueryMode(Device device, u32 mode);
void UnsetAll(Device device);
// ResetAllState: every piece of state dirty.
void ResetAllState(Device device);
// Recomputes Flags bit 0: the draws that follow run in each tile's pass.
void UpdateTilePredicated(Device device);
// True when the bound targets are the ones BeginTiling bound (or none).
bool TilingBoundSurfaces(Device device);
// What D3D does with a resource the device stops using: it is busy until the
// current fence (a recorded buffer's resource list is the GPU's; left out).
void TagResourceFence(Device device, u32 resource, bool read_fence);
// D3D's InvalidateConstants (0x82634B90) and its waiting variant (0x82640010,
// D3DDevice_BlockUntilIdle).
void InvalidateConstants(Device device);
void InvalidateConstantsAndWait(Device device);

// Clear and Resolve (d3d_native_state.cpp): the device work of the calls; the
// clear and the copy themselves are the renderer's (queued.h).
struct Rect {
  i32 x1, y1, x2, y2;
};
// D3DDevice_ClearF (0x8224F110, D3D::ClearF 0x821F2750); no rect: the bound
// target's (or the tiling extent's).
void Clear(Device device, u32 flags, const Rect *rect);
// D3DDevice_Resolve (0x821FC7A0).
void Resolve(Device device, u32 flags, u32 dest_texture, float clear_z, u32 clear_stencil,
             u32 parameters);
// D3DDevice_BeginExport (0x82237FE8) / D3DDevice_EndExport (0x82237D80): a
// memexport stream's destination, Index, is Resource.
void BeginExport(Device device, u32 index, u32 resource, u32 flags);
void EndExport(Device device, u32 index, u32 resource, u32 flags);

// blur-nx (2026-11-10/11, port 76; its key 213: blur_renut_occlusion_queries; d3d/hooks/state.cpp): an issued occlusion
// query's sample blocks answered as the reference's GPU answers them (1000 samples passed), before the game's
// D3DQuery_GetData reads them; false when the key is off or the query is not an issued occlusion query. A game wires it
// with RENUT_D3DQUERY_GETDATA_HOOK(sub_<its GetData>) in d3d/hooks/state.cpp's hook list. QueryBlockPhysical: a block's
// physical address as GetData forms it from a pointer of the query.
u32 QueryBlockPhysical(u32 pointer);
bool AnswerOcclusionQuery(u32 query);

}  // namespace renut::gpu::native
