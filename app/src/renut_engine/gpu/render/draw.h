/**
 * @file    gpu/render/draw.h
 * @brief   Guest draws, clears and resolves, recorded from the D3D device's
 *          state at the time of the call.
 *
 * The hooked D3D entry points hand over the call's arguments; everything else
 * (bound shaders, declaration, streams, textures, render targets, render state
 * and constants) is read from a snapshot of the guest D3DDevice, which the
 * game's Set* calls and inline state writes keep current: taken at the call
 * (queued.h) or when a command buffer recorded it. These run on the render
 * thread (render_thread.h) and require State().mutex.
 */
#pragma once

#include <cstring>  // SameWords

#include <string>

#include <cstdint>
#include <vector>

#include <rex/types.h>

#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/backend/video.h"

struct ShaderCacheEntry;  // banjo-nx (2026-10-27): ShaderSortableByHash

namespace renut::gpu {

// banjo-nx (2026-10-19, render-thread proposal 1, banjo_renut_store_cards): whether no page of the guest range at
// 'data' (physical memory, through any of its views) was written since interval 'proven' (CardsInterval at the time),
// reading and clearing its cards as the vertex copies' proof does; and the current interval (one per kick).
bool GuestRangeCardsClean(const u8 *data, size_t bytes, uint64_t proven);
uint64_t CardsInterval();

struct DrawCall {
  u32 primitive = 0;  // D3DPRIMITIVETYPE (Xenos primitive type)
  // Non-indexed: 'count' vertices from start_vertex.
  // Indexed: 'count' indices from start_index; vertex = base_vertex + index.
  bool indexed = false;
  u32 start_vertex = 0;
  u32 count = 0;
  i32 base_vertex = 0;
  u32 start_index = 0;

  // DrawIndexedTessellatedVertices: 'primitive' is a patch type (triangle or
  // quad patches), base_vertex the first patch and 'count' the patch count;
  // in adaptive mode the indices from start_index on are the patches' edge
  // tessellation factors. Drawn as one instance per patch
  // (renutVertexRegisters in shader_common.h).
  bool tessellation = false;

  // Draw*UP / Begin*Vertices: stream 0 comes from here instead of its fetch
  // constant, and indices (when indexed) from up_indices: copies the renderer
  // owns, taken when the game's data was complete (queued.h).
  const u8 *up_vertices = nullptr;
  u32 up_stride = 0;
  u32 up_vertex_count = 0;
  const u8 *up_indices = nullptr;
  bool up_indices_32 = false;
};

// Reads the guest vertex buffers (and vertex fetch buffers) the draws recorded
// since the last call use into their host copies. Draws leave the reads for
// the device's KickOff, which calls this, as the GPU reads them only once the
// ring is kicked; the game may write them after the draw call until then.
// Also before the command list is submitted.
void FlushVertexFillsLocked();

// banjo-nx (2026-10-27, the user's "export checks first", record-time translation): a texture fetch slot's guest
// extent (TextureGuestExtent: the base level's pages and bytes, the mip chain's), worked out by the recording threads
// (command_buffers.cpp NewOp) for the slots whose fetch words an op changed, kept per replay depth by Replay, and read
// by the fast path's export check instead of a table of answers by fetch words (render/draw.cpp FastExportReads).
struct SlotExtent {
  u32 pages[2], sizes[2];
};
struct DrawSlotExtents {
  u32 mask = 0;  // slots whose extent is known (bit r)
  SlotExtent slot[d3d::kTextureFetchSlots];
};
// Set by Replay around a replayed draw whose device's texture slots it has the extents of; null otherwise.
extern const DrawSlotExtents *g_draw_slot_extents;
bool RecordExtentsOn();

// banjo-nx (2026-10-27, the user's Phase A: resources resolved when bound, Unleashed's model where N&B allows it).
// The recording threads (command_buffers.cpp NewOp) resolve each texture slot whose address or fetch words an op
// changed to a texture object (its guest header's address and words at that moment; made on first use, shared by every
// draw that binds it), and take a draw's index buffer header words; Replay keeps them per replay depth; the render
// thread's draw reads one object per texture (render/draw.cpp FillTextures) and no guest header.
struct TextureObject;
// Any thread: the object of (va, header words), made on first use; null when the table is full (the draw then takes
// the path without objects).
TextureObject *FindTextureObject(u32 va, const u32 header[6]);
// banjo-nx (2026-10-27, geometry objects): a stream source's object by what its whole copy is resolved from (the
// stream's fetch words, its source's address, the stride byte | stream << 8, the source header's first two Format
// words), an index buffer's by its header's Common, Address and Size: the recording threads' lookups (thread-safe,
// made on first use, null when full). The render thread keeps each one's resolved copy (render/draw.cpp).
struct GeometryObject;
GeometryObject *FindStreamObject(const u32 key[6]);
GeometryObject *FindIndexObject(const u32 key[3]);
bool GeometryObjectsRecorded();  // banjo_renut_geometry_objects (the recording's side)
// banjo-nx (2026-10-28, total CPU: the recording threads' lookups): the same lookups in two steps, so a caller with
// several takes their table lines together (the misses overlap): ...Hash gives the key's hash and fetches the line of
// its first table slot, ...At looks the key up from that hash.
uint64_t TextureObjectHash(u32 va, const u32 header[6]);
TextureObject *FindTextureObjectAt(uint64_t hash, u32 va, const u32 header[6]);
uint64_t StreamObjectHash(const u32 key[6]);
GeometryObject *FindStreamObjectAt(uint64_t hash, const u32 key[6]);
uint64_t IndexObjectHash(const u32 key[3]);
GeometryObject *FindIndexObjectAt(uint64_t hash, const u32 key[3]);
bool LookupTablesOn();  // banjo_renut_record_lookups (command_buffers.cpp RecordResources' batched lookups and memos)
// banjo-nx (2026-10-28, ARM audit): Replay's stream, a stage further for the resources: an object's lines the draw reads
// (a geometry object's key line and answers, a texture object's answer lines), an op ahead; then, at the op's own
// ApplyResources (its answers came an op before), the lines of the copies its current answers name, which
// GeometryObjectsHit's proof reads first (ProveCopyRange's first load of the index copy was 1.45 % of the render thread
// in the c4 console run: a miss at nearly every draw). Prefetches only.
void PrefetchGeometryObject(const GeometryObject *o, bool index);
void PrefetchTextureObject(const TextureObject *o);
void PrefetchGeometryCopies(const GeometryObject *o, bool index);
// banjo-nx (2026-10-28, ARM audit): kWords 32-bit words equal, as 64-bit loads, XORs and ORs with one test at the end (a
// memcmp of a constant size is a call or a compare and branch a piece).
template <u32 kWords>
[[gnu::always_inline]] inline bool SameWords(const u32 *a, const u32 *b) {
  uint64_t diff = 0;
  u32 i = 0;
  for (; i + 2 <= kWords; i += 2) {
    uint64_t x, y;
    std::memcpy(&x, a + i, 8);
    std::memcpy(&y, b + i, 8);
    diff |= x ^ y;
  }
  if constexpr (kWords % 2 != 0)
    diff |= a[kWords - 1] ^ b[kWords - 1];
  return diff == 0;
}
struct DrawResources {
  TextureObject *slot[d3d::kTextureFetchSlots] = {};
  u32 mask = 0;  // slots whose object the recording gave (bit r)
  u32 ib[3] = {};  // the index buffer header's Common, Address, Size when the draw was recorded
  bool ib_valid = false;
  // The vertex buffer headers' first two Format words (CPU base, size) of the draw's bound stream sources (bit s).
  u32 vb_mask = 0;
  u32 vb[16][2] = {};
  // banjo-nx (2026-10-27, geometry objects, banjo_renut_geometry_objects): the draw's stream sources' objects (bit s of
  // obj_mask) and its index buffer's (FindStreamObject, FindIndexObject), when the recording made them.
  u32 obj_mask = 0;
  GeometryObject *streams[16] = {};
  GeometryObject *index_obj = nullptr;
};
// Set by Replay around a replayed draw; null otherwise (and with banjo_renut_resource_objects off, A/B 45).
extern const DrawResources *g_draw_resources;
bool ResourceObjectsOn();

// banjo-nx (2026-10-27, the user's shadow-caster culling; banjo_renut_shadow_cull_indices, 0 = off by default; NOT
// pixel-identical): whether Replay skips this draw: a draw of at most that many indices (or vertices) that writes depth
// only, into a square depth target (the shadow maps; the scene's depth has the screen's shape). Replay's device image.
bool ShadowCasterCulledLocked(const u8 *device_state, const DrawCall &call);
u32 ShadowCullIndices();

// The float constants of a replayed draw: the groups of four registers its
// command buffer had set by the draw (m_Pending bit 63 - group) are in the
// draw's snapshot, the rest are the running device's.
struct ReplayConstants {
  const u8 *running = nullptr;
  uint64_t vs_set = 0, ps_set = 0;
  // Groups written since the last draw of this replay that uploaded its
  // constants (same bit order), which only those need comparing: Replay
  // gathers each op's writes, the upload consumes them. 'replay' tells
  // replays apart (0: unknown, compare everything); within one the running
  // device is the same.
  uint64_t *vs_dirty = nullptr, *ps_dirty = nullptr;
  uint64_t replay = 0;
  // banjo-nx (2026-10-28, banjo_renut_replay_group_pointers): where each set group's bytes are (128 pointers: the
  // vertex shader's 64 groups, then the pixel shader's; into the recording's arena, or the device image after a sorted
  // run), instead of the device image; null: the image.
  const u8 *const *groups = nullptr;
};
// banjo-nx (2026-10-28, banjo_renut_replay_group_pointers_verify): set by Replay (the image kept as well); the reads
// that found other bytes than the image's (the 10 s "recording from marks" line).
extern bool g_group_pointers_verify;
extern uint64_t g_group_pointer_reads, g_group_pointer_differ;

// A draw from a device state snapshot (kDeviceSnapshotSize bytes laid out as
// the guest D3DDevice). Without 'replay', every float constant is the
// snapshot's.
bool DrawStateLocked(const u8 *device_state, const DrawCall &call,
                     const ReplayConstants *replay = nullptr);

// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_chain_reuse): set by backend/queued.cpp around a draw of its delta chain
// (banjo_renut_snapshot_deltas): the device chunks that changed since the chain's previous draw. The chain's chunks: the
// 30 of 64 bytes before the float constants (device bytes 0-1919), then the bool constants on: banjo-nx (N&B's
// 12,720-byte snapshot on title update 3) 41 (10112-12719, the last one 48 bytes); SA2 47 (10112-13087). 'seq' numbers the chain's draws (consecutive, from 2; a whole copy marks every chunk); 0 for
// any other draw. A memo of a draw's derived state is valid for the next draw when its seq is the memo's + 1 and none
// of the chunks it reads changed.
// banjo-nx (2026-10-26, Unleashed-style step 1): 'fields', the device fields that changed since the previous draw
// (DrawField bits), found by the recording threads word by word (command_buffers.cpp NewOp) or from the chain's chunks
// (queued.cpp): what a piece of the draw's state reads, by field, so a piece whose fields did not change keeps the
// previous draw's answer (Unleashed's dirty states).
enum DrawField : u32 {
  kFieldFetch = 1u << 0,        // the texture fetch constants, 0x480-0x6EF (banjo-nx 2026-10-27: the vertex ones apart)
  kFieldBools = 1u << 1,        // the bool and integer constants and the clip planes, 0x2780-0x287F
  kFieldDest = 1u << 2,         // the destination packet, 0x2880-0x28BF
  kFieldViewPackets = 1u << 3,  // the window, values and control packets, 0x28C0-0x2967
  kFieldMisc = 1u << 4,         // the other packets, 0x2968-0x2A7F but 0x29C0
  kField29C0 = 1u << 5,         // the word ComputeViewport reads at 0x29C0
  kFieldUnread = 1u << 6,       // 0x2A80-0x2E03: the private words no draw reads
  kFieldPrivate = 1u << 7,      // the private render state, 0x2E04-0x2F07 but the declaration
  kFieldDecl = 1u << 8,         // the vertex declaration, 0x2E30
  kFieldTail = 1u << 9,         // the rest: 0x2F08-0x309B, 0x3170-0x318B, 0x31A4-0x31AB
  kFieldIndex = 1u << 10,       // the index buffer, 0x309C
  kFieldTargets = 1u << 11,     // the render targets and the depth surface, 0x30A0-0x30B3
  kFieldStreams = 1u << 12,     // the stream sources and strides, 0x30B4-0x3107
  kFieldTexWords = 1u << 13,    // the texture words, 0x3108-0x316F
  kFieldScissor = 1u << 14,     // SetScissorRect's RECT, 0x318C-0x319B
  kFieldShaders = 1u << 15,     // the pixel and vertex shaders, 0x319C-0x31A3
  kFieldPredication = 1u << 16, // the predication, 0x31AC
  // banjo-nx (2026-10-27, render thread night): the vertex fetch constants, 0x6F0-0x77F, apart from the texture ones: a
  // draw of another mesh changes its streams' fetch constants (72-77 % of the draws), which made every such draw take
  // its textures again as if a texture had changed (Unleashed's dirty flags are per object).
  kFieldVertexFetch = 1u << 17,
  kFieldsAll = (1u << 18) - 1,
};
constexpr u32 FieldOfOffset(u32 at) {
  if (at < 0x480)
    return 0;
  if (at < 0x6F0)
    return kFieldFetch;
  if (at < 0x780)
    return kFieldVertexFetch;
  if (at < 0x2780)
    return 0;  // the float constants: by group (m_Pending)
  if (at < 0x2880)
    return kFieldBools;
  if (at < 0x28C0)
    return kFieldDest;
  if (at < 0x2968)
    return kFieldViewPackets;
  if (at >= 0x29C0 && at < 0x29C4)
    return kField29C0;
  if (at < 0x2A80)
    return kFieldMisc;
  // Title update 3's CDevice (banjo-nx 2026-10-29): 4 bytes more from 0x2AC0 and 8 from 0x2E30 (the base's 0x2E2C),
  // the same from 0x3390 (d3d.h); the base's boundaries in the comments of DrawField moved with their fields.
  if (at < 0x2DF8)  // katamari-nx: 0x2E04->0x2DF8
    return kFieldUnread;
  if (at >= d3d::kDeviceOffsetVertexDeclaration && at < d3d::kDeviceOffsetVertexDeclaration + 4)
    return kFieldDecl;
  if (at < 0x2EF8)  // katamari-nx: 0x2F08->0x2EF8
    return kFieldPrivate;
  if (at < d3d::kDeviceOffsetIndexBuffer)
    return kFieldTail;
  if (at < d3d::kDeviceOffsetRenderTargets)
    return kFieldIndex;
  if (at < d3d::kDeviceOffsetStreamSources)
    return kFieldTargets;
  if (at < d3d::kDeviceOffsetTextures)
    return kFieldStreams;
  if (at < d3d::kDeviceOffsetTextures + 4 * d3d::kTextureFetchSlots)
    return kFieldTexWords;
  if (at < 0x317C)  // katamari-nx: 0x318C->0x317C
    return kFieldTail;
  if (at < d3d::kDeviceOffsetPixelShader)
    return kFieldScissor;
  if (at < d3d::kDeviceOffsetVertexShader + 4)
    return kFieldShaders;
  if (at < d3d::kDeviceOffsetPredication)
    return kFieldTail;
  return kFieldPredication;
}
// The fields of a run of device bytes.
constexpr u32 FieldsOfRange(u32 at, u32 size) {
  u32 fields = 0;
  for (u32 w = at & ~3u; w < at + size; w += 4)
    fields |= FieldOfOffset(w);
  return fields;
}

struct ChainHint {
  uint64_t seq = 0;
  uint64_t changed[2] = {};
  u32 fields = kFieldsAll;  // banjo-nx (2026-10-26)
  // banjo-nx (2026-10-28, host packets): the replayed op's packet key (its whole device state and the call's primitive
  // kind; guest/command_buffers.cpp PacketKeyOf), 0 when it has none (an immediate draw, keys off).
  uint64_t packet_key = 0;
};
// banjo-nx (2026-10-28, host packets): a packet's first lines asked for (the replay, an op ahead); and every packet
// dropped (a shader or declaration record or a host shader went: render/draw.cpp g_packet_epoch).
void PrefetchHostPacket(uint64_t key);
void InvalidateHostPacketsLocked();
extern ChainHint g_chain_hint;
// banjo-nx (2026-10-20): the next ChainHint::seq, shared by every producer of hints (backend/queued.cpp's delta chain,
// guest/command_buffers.cpp's replays): a producer whose number is not its previous one + 1 had another draw in between
// and marks every chunk changed. banjo-nx (2026-10-27, render thread night): inline (a call into draw.cpp a draw).
inline uint64_t g_chain_seq_counter = 1;  // the first hint is 2
inline uint64_t NextChainSeq() { return ++g_chain_seq_counter; }
constexpr u32 kChainHeadChunks = 30;
constexpr u32 kChainTailBegin = 10112;
// The chain chunk of a device byte outside the float constants.
constexpr u32 ChainChunkOf(u32 device_offset) {
  return device_offset < kChainTailBegin ? device_offset / 64 : kChainHeadChunks + (device_offset - kChainTailBegin) / 64;
}
// Whether any chunk of device bytes [First, Last] changed (both outside the float constants, on the same side).
// banjo-nx (2026-10-26): the chunks as a mask made at compile time, one AND a word (a loop over the chunks before, a
// test and a branch each, at every check of every draw).
struct ChainMask {
  uint64_t m[2] = {};
};
constexpr ChainMask ChainMaskOf(u32 first, u32 last) {
  ChainMask r{};
  for (u32 c = ChainChunkOf(first); c <= ChainChunkOf(last); ++c)
    r.m[c / 64] |= uint64_t(1) << (c % 64);
  return r;
}
// banjo-nx (2026-10-26): the fields of each chain chunk, and of a chain's changed chunks (coarse: a chunk's fields).
struct ChainFieldTable {
  u32 f[128];
};
constexpr ChainFieldTable MakeChainFieldTable() {
  ChainFieldTable t{};
  for (u32 c = 0; c < 128; ++c) {
    const u32 at = c < kChainHeadChunks ? c * 64 : kChainTailBegin + (c - kChainHeadChunks) * 64;
    t.f[c] = at < d3d::kDeviceSnapshotSize ? FieldsOfRange(at, 64) : 0;
  }
  return t;
}
inline constexpr ChainFieldTable kChainFieldTable = MakeChainFieldTable();
inline u32 ChainFields(const uint64_t changed[2]) {
  u32 fields = 0;
  for (u32 w = 0; w < 2; ++w)
    for (uint64_t bits = changed[w]; bits; bits &= bits - 1)
      fields |= kChainFieldTable.f[w * 64 + u32(__builtin_ctzll(bits))];
  return fields;
}

template <u32 First, u32 Last>
inline bool ChainChanged(const ChainHint &h) {
  constexpr ChainMask m = ChainMaskOf(First, Last);
  static_assert(First <= Last && (Last < kChainTailBegin || First >= kChainTailBegin));
  return ((h.changed[0] & m.m[0]) | (h.changed[1] & m.m[1])) != 0;
}

struct ClearRect {
  i32 x1, y1, x2, y2;
};

// Predicated tiling (D3DDevice_BeginTiling .. EndTiling): the tile rects, in
// screen space; count 0 ends tiling. A draw predicated to one tile
// (d3d::kDeviceOffsetPredication) is set up for that tile's EDRAM, which the
// GPU resolves to the tile's rect: the host cuts it to the rect, offsetting
// it there unless its viewport spans the screen (see ComputeViewport).
void SetTilesLocked(const ClearRect *rects, u32 count);

// The predication mask of the RunCommandBuffer being replayed, which the
// replayed draws' own masks narrow; ~0 outside a replay. Returns the previous.
u32 SetReplayPredicationLocked(u32 mask);

// D3DDevice_ClearF. flags: 0x1-0x8 render targets 0-3, 0x10 depth, 0x20 stencil.
void ClearStateLocked(const u8 *device_state, u32 flags, const ClearRect *rect,
                      const float color[4], float depth, u32 stencil);

// D3DDevice_Resolve. flags & 7: source (0-3 render target, 4 depth);
// 0x100 clear the color target after, 0x200 clear depth/stencil after.
// 'dest_format': the destination header's fetch constant when the resolve was
// issued, or nullptr to read the header now.
struct HostTexture;

// banjo-nx (2026-10-19, banjo_renut_depth_words_lazy): 'dest' (a depth resolve destination) is read through its words:
// writes them now if its last resolve did not (from the unchanged source), and from now on at every resolve.
void EnsureDepthWordsLocked(HostTexture &dest);
// renut-ultimate (2026-11-13, sasrt-nx's resolve project R1, sasrt_gpu_diag_depth_words): the 10 s line's text (empty
// with the key off); it starts the counts again.
std::string DepthWordsCensusReport();
// vivapinata-nx (2026-11-12, vivapinata_depth_resolve_skip_unread): a depth resolve destination is read (sampled, or
// its words): its resolves are drawn from now on, and a resolve left out is drawn now.
void DepthResolveReadLocked(HostTexture &dest, int path = 0);  // path: 0 slot, 1 alias, 2 words (the log)

void ResolveStateLocked(const u8 *device_state, u32 flags, const ClearRect *source_rect,
                        u32 dest_texture_va, const d3d::TextureFetchConstant *dest_format,
                        i32 dest_x, i32 dest_y, u32 dest_level, u32 dest_slice,
                        const float clear_color[4], float clear_depth, u32 clear_stencil);

// blur-nx (2026-11-13, cycle 3: M-35 of Blur's docs/cpu-moonshots-50-1113.md): of the device a resolve reads only
// the words of SetRenderTarget's four targets and SetDepthStencilSurface's surface (kDeviceOffsetRenderTargets, then
// kDeviceOffsetDepthStencil = kDeviceOffsetRenderTargets + 16), the source as word 'flags & 7' (0-7, so eight words:
// what device.U32(kDeviceOffsetRenderTargets + source * 4) read). The queued resolve (backend/queued.cpp) keeps those
// 32 bytes instead of copying the whole 12.9 KB device; ResolveStateLocked reads them out of a whole state and runs the
// same code (exact: nothing else of the device is read).
constexpr u32 kResolveTargetWords = 8;
// blur-nx (2026-11-13, #55 of docs/cpu-near-exact-50-1113.md, exact): a clear reads of the device only the target words
// above and the viewport's four floats (x, y, width, height at kClearViewportOffset: CDevice's, as D3D clamps a clear
// to it). ClearTargetsLocked is the clear on those; ClearStateLocked reads them out of a whole state.
// renut-ultimate (2026-11-14): the offset is the XDK's (d3d.h); blur-nx's 0x3218 is past Katamari's device snapshot.
constexpr u32 kClearViewportOffset = d3d::kDeviceOffsetViewport;
static_assert(kClearViewportOffset + 16 <= d3d::kDeviceSnapshotSize);
void ReadClearViewport(const u8 *device_state, float viewport[4]);
void ClearTargetsLocked(const u32 *target_words, const float *viewport, u32 flags, const ClearRect *rect,
                        const float color[4], float depth, u32 stencil);
void ReadResolveTargets(const u8 *device_state, u32 targets[kResolveTargetWords]);
void ResolveTargetsLocked(const u32 *targets, u32 flags, const ClearRect *source_rect,
                          u32 dest_texture_va, const d3d::TextureFetchConstant *dest_format,
                          i32 dest_x, i32 dest_y, u32 dest_level, u32 dest_slice,
                          const float clear_color[4], float clear_depth, u32 clear_stencil);

// Reads render target 'source' (0-3) of the device state back for the CPU:
// what a color resolve of its rect (0, 0, width, height) into an X8R8G8B8
// texture would write, untiled, as width x height guest words (bytes
// FF RR GG BB each), row after row; the host texels a guest pixel covers at
// the target's render scale are averaged. 'width' and 'height' come back
// clamped to the target. Submits the open command list and waits for the GPU.
bool ReadRenderTargetLocked(const u8 *device_state, u32 source, u32 &width, u32 &height,
                            std::vector<u32> &words);

// What Swap scans out for the guest front buffer texture, or an empty image.
PresentImage FrontBufferImageLocked(u32 texture_va);
// Port 169 (from fnxf_xiii; banjo_renut_scaler_crop): the same, scanning out the scaler's source rect (x1 y1 x2 y2,
// guest pixels) when it is a part of the front buffer from its corner.
PresentImage FrontBufferImageLocked(u32 texture_va, const i32 scaler[4]);
// banjo-nx (2026-10-19): the texture at that address holds what a resolve wrote (not a new, uninitialised image).
bool FrontBufferWrittenLocked(u32 texture_va);

// A vertex declaration record at 'declaration_va' is being destroyed.
void ForgetVertexDeclarationLocked(u32 declaration_va);
// banjo-nx (2026-10-23, batch 6b): the 10 s report's repeat census line (banjo_renut_diag_repeat_census), or empty.
std::string RepeatCensusReport();

// banjo-nx (2026-10-26, banjo_renut_diag_unleashed_census): what Unleashed's resource model and record-time
// translation would find in N&B. Resources: the headers our Create hooks made (kind 1 vertex buffer, 2 index buffer,
// 3 texture, 4 a texture header the game had D3D_SetTextureHeader write), and the Lock calls by kind. Runs: each
// RunCommandBuffer's recordings, counted as run before (the same recording again) or not.
bool UnleashedCensusOn();
void NoteHookResource(u32 header, u8 kind);
void ForgetHookResource(u32 header);
void NoteResourceLock(u32 resource, u32 bytes);
void NoteCommandBufferRun(const void *recording, u32 draws, uint64_t swap);
std::string UnleashedCensusReport();
std::string FastCensusReport();  // banjo-nx (2026-10-26, banjo_renut_diag_fast_census)
// banjo-nx (2026-10-26, the record-time draw path): the fast draws' 10 s line (empty when none), and the executor's
// kept state dropped (a shader or vertex declaration record destroyed).
std::string FastDrawReport();
std::string TextureAnswersReport();  // banjo-nx (2026-10-27): FillTextures' answers by kind
// banjo-nx (2026-10-27, banjo_renut_diag_draw_kinds): the replay's draws by kind (shadow: a depth target and no pixel
// shader; other draws without one; colour draws), with the render thread's time from the op's state to the draw's end,
// the fields that changed, and the runs of one kind. Replay times each draw op when DrawKindsCensusOn().
bool DrawKindsCensusOn();
void NoteDrawKind(uint64_t ticks);
// banjo-nx (2026-10-27): a replayed draw that changed no field and wrote no constant since the draw before, drawing the
// indices right after its range with the same primitive and base vertex ('same_state' without the range test).
void NoteDrawMerge(bool same_state, bool contiguous);
// banjo-nx (2026-10-27, render thread night, banjo_renut_merge_draws / A/B 33): whether the device's shaders let a draw
// take the draws after it that change nothing (MergeDrawsOn): none exports, skins once, draws fur or the composite.
bool MergeDrawsOn();
bool MergeableShadersLocked(const u8 *device_state);
// banjo-nx (2026-10-27, banjo_renut_shadow_sort): whether draws of these shaders (guest addresses; pixel 0: none) may be
// replayed in another order: both known, neither exporting memory, skinning once or of the fur family, no composite.
bool SortableShadersLocked(u32 vs_va, u32 ps_va);
// The same test from a shader's hash and cache entry, on any thread (where a shader object is made).
bool ShaderSortableByHash(uint64_t hash, const ShaderCacheEntry *entry, bool pixel);
void CountMergedDraws(uint64_t draws, uint64_t ops);
std::string DrawKindsReport();
std::string DroppedDrawsReport();  // halo3-nx (2026-11-03): draws that never reached the GPU, by why
inline uint64_t RenderTicks() {
#if defined(__aarch64__)
  uint64_t t;
  asm volatile("mrs %0, cntpct_el0" : "=r"(t));
  return t;
#else
  return uint64_t(__builtin_ia32_rdtsc());
#endif
}
void InvalidateFastDrawLocked();
// banjo-nx (2026-10-28, per-draw night): a shader or vertex declaration record about to go (or a vertex shader's bound
// declaration about to be replaced): the fast path's state only when it holds that record, and the stream memo (it names
// records). The pipeline choices (cache entries, built pipelines: KeepShader keeps a record's host shader) stay.
void InvalidateFastDrawForRecordLocked(const void *record);
u32 FastTextureMaskGuess();
void PrefetchTextureMemoLocked(u32 va, const d3d::TextureFetchConstant &fetch);
// banjo-nx (2026-10-26, API-level stage 2): the 10 s "[renut] whole buffers" line; resets its counts.
std::string WholeBuffersReport();
std::string ConvertReuseReport();  // banjo-nx (2026-10-26, banjo_renut_convert_reuse)

// Once per guest Swap: ages the converted vertex stream cache and drops the
// streams no draw has read for a while.
void AdvanceStreamCacheFrameLocked();

// For the memory report: the converted vertex stream and index caches (the
// pool's host memory and what the copies in it take).
struct StreamCacheMemory {
  uint64_t pool_bytes = 0;
  uint64_t cached_bytes = 0;
  size_t streams = 0;
  size_t index_ranges = 0;
};
StreamCacheMemory StreamCacheMemoryLocked();

} // namespace renut::gpu
