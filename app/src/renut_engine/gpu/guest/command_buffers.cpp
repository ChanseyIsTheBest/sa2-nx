/**
 * @file    gpu/guest/command_buffers.cpp
 */
#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/stage_census.h"  // blur-nx (2026-11-17, the Renut audit's census)
#include "renut_engine/gpu/host_batch.h"  // blur-nx (2026-11-18, the Renut host batch)

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <thread>  // blur-nx (2026-11-17): RecordMutex's yield off the Switch
#include <unordered_map>
#include <type_traits>
#include <utility>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <vector>

#include <fmt/format.h>
#include <chrono>
#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>  // sasrt-nx (2026-11-09; port 58): Deko3DRegionGiveBack
#endif
#if defined(__SWITCH__)
#include <switch.h>  // blur-nx (2026-11-17): svcSleepThread (RecordMutex)
#include <unistd.h>  // banjo-nx (2026-10-28): sbrk (ReleaseSpareBuffersWhenHeapShort)
extern "C" char *fake_heap_end;  // libnx: the end of the malloc heap
#endif
#include <rex/frame_latency.h>  // banjo-nx (2026-10-26): the Unleashed census's frames

#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/d3d/d3d_native.h"  // banjo-nx (2026-10-26): the setters' marks (banjo_renut_tags)
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/guest/callbacks.h"  // banjo-nx (from sa2-nx): HookFastPathsOn
#include "renut_engine/gpu/backend/queued.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"
#include "renut_engine/gpu/ultimate.h"
#include "renut_engine/gpu/render/draw.h"  // banjo-nx (2026-10-20): ChainHint
#include "renut_engine/gpu/guest/resources.h"  // banjo-nx (2026-10-23): PrefetchShaderLocked
#include "renut_engine/gpu/render/texture_layout.h"  // banjo-nx (2026-10-27): the slots' extents (GetTextureDataSizes)
#include "renut_engine/gpu/render/textures.h"  // banjo-nx (2026-10-28): TextureFrameLocked (the packet census's frames)
#include "renut_engine/gpu/render/surfaces.h"  // blur-nx (2026-11-10, port 76; its key 215): StackHeaderSet

#include <rex/cvar.h>

// banjo-nx (2026-10-26): recording from the setters' marks (d3d_native.h).
// blur-nx (2026-11-13, NewOp's scan: the nx race profile had NoteSetWords at about a third of NewOp, ~1.2 ms a frame on
// JQ Workers 0 / 1, the frame chain): an op scans for words set since Begin only the 128-byte blocks holding a chunk
// that changed since the device's previous op (the op's 'changed', which the replay relies on as well). Exact:
// set_words only accumulates, and a chunk that did not change still differs from the Begin image where it did at the
// previous op, already noted; a buffer's first op scans everything.
// blur-nx (2026-11-17, the 24-hour run): the record mutex as a spin-then-yield lock (RecordMutex below).
REXCVAR_DEFINE_BOOL(blur_renut_record_spin, true, "Banjo/Enhancements",
                    "The command buffers' record mutex (every recorded op's NewOp when its memo misses, BeginCommandBuffer, "
                    "RunCommandBuffer) as a lock that spins briefly, then yields, then naps, instead of Horizon's mutex, "
                    "which hands a released lock to a waiting thread even when that thread is not running (the frame "
                    "jobs waited 12 % of their time on it while its holders were almost never sampled inside). Off: "
                    "the system mutex, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx (2026-11-17, the 24-hour run): RunCommandBuffer's runs from a pool (QueueRunCommandBuffer, NewRun).
REXCVAR_DEFINE_BOOL(blur_renut_run_pool, true, "Banjo/Enhancements",
                    "Each D3DDevice_RunCommandBuffer's run (its 12.9 KB copy of the running device and its buffers) "
                    "taken from a pool of up to 64 the render thread gives back, instead of a new allocation under "
                    "the one malloc lock (Game Master spent 0.8 % of its racing time in memalign there). Off: a new "
                    "one each time, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_replay_pool, true, "Banjo/Renut",
                    "blur-nx (the Renut audit's #25/#26, exact): a run's shared_ptr control block comes from a free "
                    "list and its replay's closure is a pooled job (one pointer, kept in the std::function itself), "
                    "not two heap allocations under the malloc lock each RunCommandBuffer. false: as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx (2026-11-17, the 24-hour run): every hook's first steps (FlushRecordedVertices here, FlushDeferredVertices in
// backend/queued.cpp) take their locks only when their own device has something waiting.
REXCVAR_DEFINE_BOOL(blur_renut_flush_fast_local, true, "Banjo/Enhancements",
                    "Every D3D hook's first steps (the recorded and the deferred Begin*Vertices data) take their locks "
                    "only when their own device has something waiting, not whenever any device has (Game Master waited "
                    "3.4 % of its racing time on the delta chain's lock in QueueDraw). Off: the global counts, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx (2026-11-17, the 24-hour run): NewOp's memo checked per command buffer and per device (below, RecordCache).
REXCVAR_DEFINE_BOOL(blur_renut_record_memo_local, true, "Banjo/Enhancements",
                    "NewOp's lock-free path (banjo_renut_cpu_memos) invalidated per command buffer and per device: a "
                    "buffer begun or released elsewhere, or vertices pending for another device, no longer send every "
                    "recording thread through the record mutex (the job workers waited on it 7 % of their time). Off: "
                    "the global generation and pending count, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_set_words_changed, true, "Banjo/Enhancements",
                    "Renut's command buffer recording: an op notes the words set since Begin (halo3-nx's inherit) from "
                    "the chunks that changed since its device's previous op only (exact). false: every op scans the "
                    "fetch constants and the tail")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_set_words_verify, false, "Banjo/Debug",
                    "banjo_renut_set_words_changed's check: every op also scans everything and counts the words the "
                    "changed chunks missed (logged; the full scan's result kept)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_tags, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-26): a recorded op compares only the device chunks the native D3D "
                    "setters marked as written since the device's previous op, not all 53 (the same op, the same "
                    "bytes: the rest are unchanged). Off: every chunk compared, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, banjo_renut_host_packets);  // banjo-nx (2026-10-28): render/draw.cpp
REXCVAR_DEFINE_BOOL(banjo_renut_diag_packet_census, false, "Banjo/Diagnostics",
                    "Renut's renderer (2026-10-28, host draw packets): every 10 s, how many replayed draws a cache keyed "
                    "by the draw's whole device state (not the float constants) would answer, and how many of those "
                    "re-derive state today. Hashes 3 KB a draw: diagnostics only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_renut_tags_check, 32, "Banjo/Enhancements",
                     "Renut's renderer (banjo_renut_tags): every Nth op of a recording device also compares every "
                     "chunk; one changed without a mark (a write the setters do not see) goes into the op, is logged, "
                     "and the marks are not used again. 1: the verify form, every op checked, the marks kept, every "
                     "such chunk counted and logged. 0: never checked")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-20): sa2-nx's chain reuse (render/draw.cpp, banjo_renut_chain_reuse) for the replayed draws too.
REXCVAR_DECLARE(bool, banjo_renut_diag_fast_census);  // banjo-nx (2026-10-26): render/draw.cpp
// renut-ultimate (2026-11-11, port 77; the user's rule: a key that breaks graphics in a specific game is off by
// default): on, vivapinata-nx (which records on its real device) got a hard shadow under its title logo (2,378 dark
// grass pixels at 21 s against 0 off and on the SDK's emulated GPU). halo3-nx, and any game that records on a device of
// its own, sets it true.
REXCVAR_DEFINE_BOOL(banjo_renut_replay_inherits, false, "Banjo/Renut",
                    "halo3-nx (2026-11-03): a replayed draw is the running device with only what its recording set, as "
                    "on the console: a command buffer holds what was set in it and inherits the rest. Off: each recording's "
                    "whole device, which suits a game recording on its real device, and drops every draw of one "
                    "recording on a device of its own (Halo 3's no-hardware device has no targets)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(vivapinata_tiles_once, true, "Banjo/Enhancements",
                    "vivapinata-nx (2026-11-11, the GPU round; A/B 86): a recorded command buffer the game runs once per "
                    "predicated tile (SetPredication(tile), RunCommandBuffer, the same buffer for the next tile) is "
                    "replayed once with every tile's predication. The host draws into one full-size target, so each "
                    "draw then covers the tiles' rects in one go instead of one per run: half the vertex, tessellation "
                    "and memexport work of those draws, the same pixels (the rects are disjoint and each pixel gets the "
                    "same draws in the same order). Only when the runs are adjacent, of the same captured recordings, "
                    "with no clear or resolve in them, and the running devices are the same but for the predication; "
                    "anything else is replayed per tile as before. Off: per tile")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_diag_replay_state, false, "Banjo/Diagnostics",
                    "Diagnostic (halo3-nx): logs the first replayed draws' device words that differ from the running "
                    "device's, what each recording overrides");
REXCVAR_DEFINE_BOOL(banjo_renut_chain_reuse_replays, true, "Banjo/Enhancements",
                    "OFF by default since package 36r, with banjo_renut_chain_reuse. Renut's renderer (banjo_renut_chain_reuse): the draws replayed from the game's recorded command "
                    "buffers (about three quarters of N&B's draws in play) tell the draw which device chunks their ops "
                    "changed since the replay's previous draw, so it takes that draw's pipeline key, targets, viewport, "
                    "texture words and pipeline when nothing they are made from changed (sa2-nx's chain reuse is for the "
                    "immediate draws only). banjo_renut_chain_reuse_verify checks these too. Off: replayed draws make "
                    "them all again")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// banjo-nx (2026-10-27, the shadow sort; the user's request after the render thread night).
// OFF by default since the console's city run of 2026-10-27 (builds/2026-10-27_1047_shadowsort, handheld, stock clocks,
// A/B 40 over 2 periods): render thread +1.13 ms a frame (56.72 sorted against 55.59 as recorded) and GPU +0.65 ms
// (42.06 against 41.42). Exact (verify: millions of devices compared, 0 differ), and it cut the shader changes inside
// the shadow runs by about half on the console (70 % on the emulator's title), but: the game's own order is close to
// front to back, which the depth test's early rejection likes, and sorting by shader throws that away (the GPU's
// loss); in the city the runs are short (an unsortable draw ends most of them: 35 % of the draws sortable, about 155 k
// a 10 s window refused by their shaders); and each sorted draw copies its device bytes from all over the run's arena,
// where the recorded order reads one op's bytes in a row. The emulator's census: 7.39 us a shadow draw sorted against
// 7.48 as recorded (an even trade there). Kept for its A/B (target 40, with this key on) and as the base of a sort that
// keeps the front-to-back order within each shader (not tried).
// SLOWER ON THE CONSOLE (A/B 40, 2026-10-27 city: render thread +1.1 ms, GPU +0.65 ms a frame): off by default.
REXCVAR_DEFINE_BOOL(banjo_renut_shadow_sort, false, "Banjo/Enhancements",
                    "SLOWER ON THE CONSOLE, OFF by default (2026-10-27 console run: render thread +1.1 ms, GPU +0.65 ms a frame). "
                    "Renut's renderer (2026-10-27): the draws of a recorded command buffer that write depth only (a depth "
                    "target, no colour written, no stencil, a less / less-equal or greater / greater-equal depth test) "
                    "and follow one another into the same targets are replayed sorted by vertex shader, pixel shader and "
                    "declaration: such draws leave the same depth in any order (each sample keeps the nearest), and "
                    "sorted they change shaders and pipelines far less often. A run whose shaders export memory, skin "
                    "once or are the fur family is replayed as recorded. In-run A/B target 40. Read at start")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-27): the replay's look-ahead (2026-10-23 / 2026-10-26: while a draw is drawn, the next op's
// shader lookups, guest headers and texture memo entries are prefetched from its chunks in the arena). OFF by default
// since the console's city run of 2026-10-27 (A/B 29, one period): render thread 55.88 ms a frame with it against 53.73
// without (+2.15 ms, +4.0 %). Its loads were cheap on the emulator and their misses were most of a draw's loads in the
// 2026-10-23 profile, but on the A57 the peeks (PeekNextWord: seven words a texture slot, the header chunks' words, a
// memo entry hashed per slot) cost more than the misses they hide: the draw's own prefetches (FastDrawLocked,
// WholeGeometryTry) already reach most of those lines a few hundred cycles ahead.
// SLOWER ON THE CONSOLE (A/B 29, 2026-10-27 city: render thread +2.15 ms a frame; confirmed by night_gpu5's check,
// four-core, 30 s halves, the control at +0.3 ms: +2.79 ms, +3.1 / +2.2 / +3.1): off by default.
REXCVAR_DEFINE_BOOL(banjo_renut_replay_lookahead, false, "Banjo/Enhancements",
                    "SLOWER ON THE CONSOLE, OFF by default (2026-10-27 console run: render thread +2.15 ms a frame with it). Renut's renderer "
                    "(2026-10-23): while a replayed draw is drawn, the next op's shader lookups, guest headers and texture "
                    "memo entries are prefetched from its recorded chunks. In-run A/B target 29 with this key on")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-28): NO MEASURABLE EFFECT (not slower): night_final's +0.99 ms was one sample inside the noise;
// night_gpu5's check (four-core, 30 s halves, the control at +0.3 ms) had -0.21 ms (-0.6, 0.0, 0.0). Off (no gain).
REXCVAR_DEFINE_BOOL(banjo_renut_replay_record_prefetch, false, "Banjo/Enhancements",
                    "OFF by default: no measurable effect (night_gpu5 console check, target 57: -0.21 ms a frame, within "
                    "the noise; night_final's +0.99 ms was noise). Renut's renderer (2026-10-28, per-draw night): the replay prefetches the "
                    "next op's shader and declaration records (at their own guest addresses) from its recorded words. "
                    "In-run A/B target 57 with this key on")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_replay_stream, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-27, the replay stream): recorded ops are 64-byte aligned and a draw's op "
                    "is two lines (its call without pointers, the UP data by offset, the shaders' words as recorded); "
                    "while an op is replayed, the op two ahead's lines and the next op's arena state, resource and "
                    "extent records and (other shaders) shader lookups are prefetched from recorded values, nothing "
                    "decoded. Exact (prefetches only). In-run A/B target 46")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_replay_stream_shaders, false, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-27, with banjo_renut_replay_stream): the shader memo's sets prefetched "
                    "two ops ahead and the shader records one op ahead (a look in the set fetched an op before); off: "
                    "the sets one op ahead. Exact (prefetches only). In-run A/B target 49. OFF by default since the c4 "
                    "console run (2026-10-27): +0.7 ms a frame with it, 4 of 5 periods worse")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_first_op_groups, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-28, cache-miss audit): a recorded buffer's first op keeps the float "
                    "constant groups the recording has set so far, not all 512 registers (8 KB copied into the arena "
                    "on the recording thread and again into the replay's device image on the render thread, for "
                    "every buffer). A replayed draw reads a group from the image only when its op's set mask holds it "
                    "(the others from the running device), and every such group was copied by an op at or before it. "
                    "Exact. In-run A/B target 51")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_replay_group_pointers, true, "Banjo/Enhancements",
                    "Renut's renderer (2026-10-28, per-draw night): a replay keeps a pointer to each float constant "
                    "group's bytes where the op recorded them (the arena), and a draw reads its set groups there, "
                    "instead of copying every op's groups into the device image (64 bytes a group, the image's lines "
                    "touched again by the draw). A nested run gets its set groups written into the image first. The "
                    "same bytes. In-run A/B target 52")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_replay_group_pointers_verify, false, "Banjo/Debug",
                    "Debug (banjo_renut_replay_group_pointers): the groups are copied into the device image as well, and "
                    "every read through a group pointer is compared with the image's bytes; the count of reads that "
                    "differ is in the 10 s \"recording from marks\" line")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_shadow_sort_verify, false, "Banjo/Debug",
                    "Renut's renderer (banjo_renut_shadow_sort): each sorted run's ops are also applied as recorded "
                    "and the device each sorted draw (and the run's end) is drawn from is compared whole with the one "
                    "its op had as recorded; a difference is logged and counted in the shadow sort line. Slow")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace renut::gpu {

namespace {

constexpr size_t kNone = SIZE_MAX;
// RunCommandBuffer inside a recorded buffer, and so on.
constexpr u32 kMaxRunDepth = 8;

constexpr u32 kVertexShaderF =
    u32(offsetof(d3d::Device, m_Constants) + offsetof(d3d::Constants, VertexShaderF));
constexpr u32 kPixelShaderF =
    u32(offsetof(d3d::Device, m_Constants) + offsetof(d3d::Constants, PixelShaderF));
// One m_Pending bit per 4 float constants, most significant bit first
// (SetPending_AluConstants 0x821FC1C8).
constexpr u32 kConstantGroupBytes = 4 * 16;

// What an op keeps of the device (see Op::state). The draw, clear and resolve
// paths read nothing before the fetch constants (dirty masks, ring pointers,
// the Set/Get*State call tables), and the float constants, two thirds of the
// snapshot, are kept as the groups written since the device's previous op.
constexpr u32 kFetchBegin = u32(offsetof(d3d::Device, m_Constants));
constexpr u32 kFetchBytes = kVertexShaderF - kFetchBegin;
constexpr u32 kFloatBytes = 2 * 256 * 16;
constexpr u32 kTailBegin = kVertexShaderF + kFloatBytes;
constexpr u32 kTailBytes = d3d::kDeviceSnapshotSize - kTailBegin;
static_assert(kPixelShaderF == kVertexShaderF + 256 * 16);
static_assert(kTailBegin == u32(offsetof(d3d::Device, m_Constants) + offsetof(d3d::Constants, VertexShaderB)));

// The rest, the fetch constants and the tail, in 64-byte chunks: an op keeps
// the chunks that differ from the device's previous op (Op::plain_dirty, bit
// i = chunk i), which for most draws are a few bindings and render states.
// Replaying them was a copy of the whole of both per draw on the render
// thread; comparing them is the recording threads' work.
constexpr u32 kChunkBytes = 64;
constexpr u32 kFetchChunks = kFetchBytes / kChunkBytes;
constexpr u32 kTailChunks = (kTailBytes + kChunkBytes - 1) / kChunkBytes;
constexpr u32 kPlainChunks = kFetchChunks + kTailChunks;
static_assert(kFetchBytes % kChunkBytes == 0 && kPlainChunks <= 64);
constexpr uint64_t kAllChunks = kPlainChunks == 64 ? ~uint64_t(0) : (uint64_t(1) << kPlainChunks) - 1;

// Chunk i's offset in the device, and its size (the tail's last is short).
// banjo-nx (2026-10-20, banjo_renut_chain_reuse_replays): an op's chunks (plain_dirty) as draw.h's ChainHint chunks:
// the fetch constants' twelve are the chain's 18-29 (device bytes 0x480-0x77F), the tail's are the chain's from 30.
constexpr u32 kChainFirstFetchChunk = kFetchBegin / kChunkBytes;
static_assert(kFetchBegin % kChunkBytes == 0 && kChainFirstFetchChunk + kFetchChunks == kChainHeadChunks &&
              kTailBegin == kChainTailBegin && kChainHeadChunks + kTailChunks <= 128);
// banjo-nx (2026-10-26): as shifts (a loop over the chunks before): the fetch constants' up by 18, the tail's from 30.
static_assert(kFetchChunks == 12 && kChainFirstFetchChunk == 18 && kChainHeadChunks == 30 &&
              kChainHeadChunks + kPlainChunks - kFetchChunks <= 128);
void PlainToChain(uint64_t plain, uint64_t out[2]) {
  const uint64_t tail = plain >> kFetchChunks;
  out[0] = (plain & ((uint64_t(1) << kFetchChunks) - 1)) << kChainFirstFetchChunk | tail << kChainHeadChunks;
  out[1] = tail >> (64 - kChainHeadChunks);
}

constexpr u32 ChunkOffset(u32 i) {
  return i < kFetchChunks ? kFetchBegin + i * kChunkBytes
                          : kTailBegin + (i - kFetchChunks) * kChunkBytes;
}
// banjo-nx (2026-10-23, batch 6): the tail's last chunk, the only one not 64 bytes: its copies as a fixed-size copy the
// compiler keeps inline (a C library call each before; 1.3 % of the render thread at ApplyState's).
constexpr u32 kLastChunkBytes = kTailBytes - (kTailChunks - 1) * kChunkBytes;
static_assert(kLastChunkBytes > 0 && kLastChunkBytes <= kChunkBytes);

constexpr u32 ChunkSize(u32 i) {
  return i < kFetchChunks ? kChunkBytes
                          : std::min(kChunkBytes, kTailBytes - (i - kFetchChunks) * kChunkBytes);
}

// Calls f(first, count) for each run of consecutive float constant groups in
// an m_Pending mask (bit 63 - group).
template <typename F>
void ForEachGroupRun(uint64_t mask, F f) {
  while (mask) {
    const u32 first = u32(std::countl_zero(mask));
    const u32 count = u32(std::countl_one(mask << first));
    f(first, count);
    mask = first + count >= 64 ? 0 : mask & (~uint64_t(0) >> (first + count));
  }
}

enum class OpKind { Draw, Clear, Resolve, Run };

// banjo-nx (2026-10-27, the replay stream, banjo_renut_replay_stream): a draw's call as its op keeps it: DrawCall's
// values without its pointers (Draw*UP / Begin*Vertices data are arena offsets, kNoUp none) and the shaders' words as
// recorded (Replay prefetches the next draw's shader lookups from them). An op is 64-byte aligned and a draw reads only
// its first two lines: the first (kind .. the dirty masks) and this. (Before: the DrawCall's pointers took the second
// line and the arena offsets were on the third; with a 288-byte op every other op started mid-line, so a draw read up
// to four lines, each a miss: the console's b35 profile had three loads of the op's lines at 4 % of the render thread.)
constexpr u32 kNoUp = 0xFFFFFFFFu;
struct OpCall {
  u32 primitive = 0;
  bool indexed = false, tessellation = false, up_indices_32 = false;
  u32 start_vertex = 0, count = 0;
  i32 base_vertex = 0;
  u32 start_index = 0;
  u32 up_stride = 0, up_vertex_count = 0;
  u32 up_vertices = kNoUp, up_indices = kNoUp;  // arena offsets
  u32 vs_word = 0, ps_word = 0;                 // the device's shader words when recorded (prefetch only)
  u32 decl_word = 0;  // banjo-nx (2026-10-28): the device's vertex declaration word when recorded (prefetch only)
  // banjo-nx (2026-10-28, host packets, banjo_renut_host_packets): the op's packet words as a key (the device words a
  // packet's answers read: kPacketWords), 0 when not made. NewOp keeps it per device as the XOR of each chunk's mixed
  // hash, so an op hashes only the chunks it copies that hold such words (PacketChunkPart).
  uint64_t state_key = 0;
};
static_assert(sizeof(OpCall) <= 64);

struct alignas(64) Op {
  OpKind kind = OpKind::Draw;
  // banjo-nx (2026-10-27, Phase A): the arena offset of the op's resource record (RecordResources), or 0xFFFFFFFF.
  // In the padding after 'kind'.
  u32 resources = 0xFFFFFFFFu;
  // The device in the arena (CopyState): the chunks in plain_dirty, then the
  // float constant groups in vs_dirty and ps_dirty. Replay rebuilds the whole
  // snapshot from the previous op's (ApplyState).
  size_t state = kNone;
  // Chunks that differ from the device's previous op; all for a buffer's first.
  uint64_t plain_dirty = kAllChunks;
  // banjo-nx (2026-10-26, Unleashed-style step 1): the device fields (draw.h DrawField) that differ from the device's
  // previous op, found word by word; all for a buffer's first.
  u32 fields = kFieldsAll;
  // banjo-nx (2026-10-27, banjo_renut_record_extents): the arena offset of the extents of the texture slots this op's
  // chunks changed (RecordExtents), or kNoExtents (0xFFFFFFFF). Next to 'fields', in its padding: Replay reads it with
  // the op's first line.
  u32 extents = 0xFFFFFFFFu;
  // Float constant groups the buffer had set by this op (m_Pending[0] / [1]
  // bits); the rest are inherited from the running device.
  uint64_t vs_set = 0, ps_set = 0;
  // Those written since the device's previous op (ReplayConstants::vs_dirty),
  // all for a buffer's first op. Only the pending-mask consumers (the draw
  // calls, RunCommandBuffer and EndCommandBuffer, all but the last hooked)
  // clear them, so the others hold what the previous op of the buffer had.
  uint64_t vs_dirty = ~uint64_t(0), ps_dirty = ~uint64_t(0);

  // Draw (the op's second line)
  OpCall call;

  // Clear / Resolve
  u32 flags = 0;
  bool has_rect = false;
  ClearRect rect{};
  bool has_color = false;
  float color[4] = {};
  float depth = 0.0f;
  u32 stencil = 0;
  u32 dest_texture = 0;
  // The destination header when recorded; the game often builds it on the
  // recording thread's stack.
  bool has_dest_format = false;
  d3d::TextureFetchConstant dest_format{};
  i32 dest_x = 0, dest_y = 0;
  u32 dest_level = 0, dest_slice = 0;

  // Run
  u32 command_buffer = 0;

  u32 binding = 0;  // halo3-nx (2026-11-03, banjo_renut_recorded_binds): a draw's shader's bind (LastBindingOf)

  // banjo-nx (2026-10-27, banjo_renut_shadow_sort): a draw that writes depth only (SortFacts): its key (vertex shader
  // << 32 | pixel shader, then the declaration) and its run's signature (the targets and the depth test's family);
  // sort_sig 0: not sortable, sort_why the first reason (the census).
  uint64_t sort_key = 0;
  u32 sort_decl = 0, sort_sig = 0;
  u32 sort_vs = 0, sort_ps = 0;  // the shaders' guest addresses (the key holds their contents' digests)
  u32 sort_rs = 0;  // banjo-nx (2026-10-27): a digest of the render state the pipeline is made from (the second level)
  u32 sort_tex = 0;  // and of the first texture slots' addresses (the third: alpha-tested casters share shaders, not textures)
  u8 sort_why = 0;
};
static_assert(offsetof(Op, call) == 64 && alignof(Op) == 64, "a draw's op is its first two lines");
constexpr u32 kNoExtents = 0xFFFFFFFFu;

// The DrawCall a recorded draw replays (its UP data in the buffer's arena).
inline DrawCall CallOf(const OpCall &c, const u8 *arena) {
  DrawCall d;
  d.primitive = c.primitive;
  d.indexed = c.indexed;
  d.start_vertex = c.start_vertex;
  d.count = c.count;
  d.base_vertex = c.base_vertex;
  d.start_index = c.start_index;
  d.tessellation = c.tessellation;
  d.up_vertices = c.up_vertices != kNoUp ? arena + c.up_vertices : nullptr;
  d.up_stride = c.up_stride;
  d.up_vertex_count = c.up_vertex_count;
  d.up_indices = c.up_indices != kNoUp ? arena + c.up_indices : nullptr;
  d.up_indices_32 = c.up_indices_32;
  return d;
}
// A draw call into its op (the UP data's offsets set by the caller).
inline void SetOpCall(OpCall &o, const DrawCall &c, const u8 *device) {
  o.primitive = c.primitive;
  o.indexed = c.indexed;
  o.start_vertex = c.start_vertex;
  o.count = c.count;
  o.base_vertex = c.base_vertex;
  o.start_index = c.start_index;
  o.tessellation = c.tessellation;
  o.up_stride = c.up_stride;
  o.up_vertex_count = c.up_vertex_count;
  o.up_indices_32 = c.up_indices_32;
  o.up_vertices = o.up_indices = kNoUp;
  if (device) {
    u32 v;
    std::memcpy(&v, device + d3d::kDeviceOffsetVertexShader, 4);
    o.vs_word = __builtin_bswap32(v);
    std::memcpy(&v, device + d3d::kDeviceOffsetPixelShader, 4);
    o.ps_word = __builtin_bswap32(v);
    std::memcpy(&v, device + d3d::kDeviceOffsetVertexDeclaration, 4);
    o.decl_word = __builtin_bswap32(v);
  }
}

// banjo-nx (2026-10-27, setter rewrite): the arena's growth leaves its new bytes as they are: every op's state is copied
// in whole right after (CopyState), and zeroing them first was a second write of the op's bytes on the recording threads
// (their memset). Reserve zeroes what it hands out, as before.
template <typename T>
struct DefaultInitAllocator : std::allocator<T> {
  template <typename U>
  struct rebind {
    using other = DefaultInitAllocator<U>;
  };
  DefaultInitAllocator() noexcept = default;
  template <typename U>
  DefaultInitAllocator(const DefaultInitAllocator<U> &) noexcept {}
  template <typename U>
  void construct(U *p) noexcept(std::is_nothrow_default_constructible_v<U>) {
    ::new (static_cast<void *>(p)) U;
  }
  template <typename U, typename... Args>
  void construct(U *p, Args &&...args) {
    ::new (static_cast<void *>(p)) U(std::forward<Args>(args)...);
  }
};

// banjo-nx (2026-10-27, banjo_renut_shadow_sort): a run's draws in their sorted order, each with the device bytes that
// differ from the draw before it in that order: pairs of a slot (0-52 a plain chunk, 64 + g a vertex shader constant
// group, 128 + g a pixel shader one) and where its bytes are in the arena (an op's copy). The last entry draws nothing:
// it brings the device back to the run's last op as recorded, from which the next op's own delta goes on.
struct SortPair {
  u32 slot, offset;
};
constexpr u32 kSortRestore = UINT32_MAX;
struct SortEntry {
  u32 op = kSortRestore;  // the op drawn, kSortRestore for the run's last entry
  u32 first_pair = 0, pair_count = 0;
  u32 fields = 0;  // DrawField bits of the chunks changed (whole chunks)
  uint64_t plain = 0, vs_dirty = 0, ps_dirty = 0;
};
struct SortRun {
  u32 begin = 0, end = 0;  // the ops [begin, end) as recorded
  u32 first_entry = 0, entry_count = 0;
};
inline bool ShadowSortOn() {
  static const bool on = REXCVAR_GET(banjo_renut_shadow_sort);
  return on;
}
// banjo-nx (2026-10-27): banjo_renut_replay_lookahead (off: see its definition), and not in A/B 29's off half.
inline bool ReplayLookAheadOn() {
  static const bool on = REXCVAR_GET(banjo_renut_replay_lookahead);
  return on && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 29;
}
// banjo-nx (2026-10-27): banjo_renut_replay_stream, and not in A/B 46's off half. The bytes of an op's arena state
// prefetched (eight lines: an op's state is about 500 bytes in the city, 3.1 chunks and 4.7 constant groups).
inline bool ReplayStreamOn() {
  static const bool on = REXCVAR_GET(banjo_renut_replay_stream);
  return on && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 46;
}
// banjo-nx (2026-10-27): the stream's shader stages (memo sets two ahead, records one ahead), and not in A/B 49's off
// half. (Without them: the memo sets one ahead, as in c2.)
inline bool ReplayStreamShadersOn() {
  static const bool on = REXCVAR_GET(banjo_renut_replay_stream_shaders);
  return on && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 49;
}
constexpr u32 kStreamStateBytes = 512;
// The replay's counts (render thread).
uint64_t g_sort_replayed_runs = 0, g_sort_replayed_draws = 0, g_sort_refused_runs = 0;
uint64_t g_sort_verified = 0, g_sort_verify_differ = 0;  // banjo_renut_shadow_sort_verify's

struct HostCommandBuffer {
  std::vector<Op> ops;
  std::vector<u8, DefaultInitAllocator<u8>> arena;
  uint64_t id = 0;  // banjo-nx (2026-10-26): a number per recording (the objects are pooled), for the census
  // halo3-nx (2026-11-03, banjo_renut_replay_inherits): the device words this recording set, a bit a word, against the
  // device at its Begin; the ranges it did not set, which a replay takes from the running device (BuildInheritRuns).
  // renut-ultimate (port 67, 2026-11-10, blur-nx): rounded up. NoteSetWords and BuildInheritRuns index it with at / 128
  // up to kDeviceSnapshotSize, so a snapshot that does not end on 128 bytes read and wrote its last words one past the
  // end, in inherit_runs (Katamari's and RR6's 12,704 bytes: the last 32 words; Blur's 12,888: the last 88, and its
  // render thread crashed in BuildInheritRuns at the first replay).
  u32 set_words[(d3d::kDeviceSnapshotSize / 4 + 31) / 32] = {};
  std::vector<std::pair<u32, u32>> inherit_runs;  // offset, bytes
  bool inherit_built = false;
  // blur-nx (2026-11-18, the Renut host batch's #3, blur_renut_inherit_by_chunk): the same runs cut at the chunks'
  // edges, in chunk order, and each chunk's first run (inherit_chunk_at[i] .. [i + 1]); their bytes, for the census.
  std::vector<std::pair<u32, u32>> inherit_chunk_runs;
  std::array<uint16_t, kPlainChunks + 1> inherit_chunk_at{};
  bool inherit_chunk_ready = false;
  u32 inherit_bytes = 0;
  // renut-ultimate (audit, 2026-11-07, ultimate_renut_replay_inherit_same): the inherited runs' bytes as op 0 recorded them
  // (every op had them so), one after another; empty when unknown.
  std::vector<u8> inherit_recorded;
  // banjo-nx (2026-10-27, banjo_renut_shadow_sort): the sorted runs (PlanSortedRuns, by the thread that queues the
  // buffer's run: 0 none yet, 1 being made, 2 made; the render thread reads them only at 2).
  std::vector<SortRun> sort_runs;
  std::vector<SortEntry> sort_entries;
  std::vector<SortPair> sort_pairs;
  std::atomic<u32> sort_state{0};
  // blur-nx (2026-11-10, port 76; its key 215: blur_renut_stack_surface_headers): the ops recorded with a stack surface
  // bound as a target, by op index (ascending), with that surface's header as it was then: a recording replays long
  // after its function returned.
  std::vector<std::pair<u32, StackHeaderSet>> stack_headers;
  void ClearSortPlan() {  // (called where a buffer starts a new recording: the stack headers go with the old ops too)
    sort_runs.clear();
    sort_entries.clear();
    sort_pairs.clear();
    sort_state.store(0, std::memory_order_relaxed);
    stack_headers.clear();
  }
};
std::atomic<uint64_t> g_recording_ids{0};

// Vertex data of a recorded Begin*Vertices draw, still being written.
struct PendingVertices {
  u32 device = 0;
  u32 command_buffer = 0;
  size_t offset = 0;
  const u8 *source = nullptr;
  u32 size = 0;
};

// banjo-nx (2026-10-26, banjo_renut_tags): NewOp's counts of the marks (TagsReport).
struct TagCounts {
  uint64_t ops = 0, marked_ops = 0, marked = 0, changed = 0, checks = 0;
  // banjo-nx (2026-10-26, banjo_renut_diag_fast_census: how much of the state comes through the hooked setters): the
  // changed chunks a setter marked and those only an inline write's pending tag marked, by the fields they hold.
  uint64_t setter_changed = 0, inline_changed = 0;
  uint64_t setter_fields[18] = {}, inline_fields[18] = {};
  // banjo-nx (2026-10-26, setter rewrite): the marks by source (the setters' touches, the tags' chunks no touch marked)
  // and the op counts they are over.
  uint64_t touch_marks = 0, tag_only_marks = 0, census_ops = 0;
};
std::atomic<uint64_t> g_census_setter_changed{0}, g_census_inline_changed{0};
std::atomic<uint64_t> g_census_touch_marks{0}, g_census_tag_only_marks{0}, g_census_ops{0};
std::atomic<uint64_t> g_census_setter_fields[18], g_census_inline_fields[18];

// What NewOp keeps per recording device.
struct DeviceRecording {
  // Float constant groups the buffer has set (Op::vs_set / ps_set).
  uint64_t vs = 0, ps = 0;
  // blur-nx (2026-11-17): this device's entries in g_pending_vertices (changed under g_record_mutex; NewOp's memo reads
  // it without the lock: blur_renut_record_memo_local).
  std::atomic<u32> pending{0};
  // banjo-nx (2026-10-26, banjo_renut_tags): the shadow is the device's as of its last op and the device's marks have
  // been taken since (the next op may compare only the marked chunks); ops since the last whole check.
  bool primed = false;
  u32 unchecked = 0;
  TagCounts counts;
  // The device as its last op saw it, in the device layout; only the chunks
  // (ChunkOffset) are kept.
  u8 shadow[d3d::kDeviceSnapshotSize];
  u8 begin_image[d3d::kDeviceSnapshotSize];  // halo3-nx: the device at the buffer's Begin (HostCommandBuffer::set_words)
  // banjo-nx (2026-10-28, host packets): each chunk's mixed hash as the ops last copied it, and their XOR (the state key
  // of the device's last op). Zero together at the start (the XOR of zeros); a buffer's first op copies every chunk.
  uint64_t chunk_key[64] = {};
  uint64_t state_key = 0;
};

// A RunCommandBuffer's buffer and every buffer it runs in turn, as they were
// when it was issued: the render thread replays it later, by when the game
// may be recording the next frame's into the same D3DCommandBuffers.
using BufferSet = std::unordered_map<u32, std::shared_ptr<HostCommandBuffer>>;

// blur-nx (2026-11-17, the 24-hour run; blur_renut_record_spin): the record mutex. The game threads that record
// (the frame jobs, Game Master) take it some 30,000 times a second for a microsecond or so; Horizon's mutex parks a
// waiter in the kernel and, at the unlock, hands the lock to it, so the lock then belongs to a thread that may wait
// for its core while the others wait for it. This lock never hands over: a waiter spins a little (the holder is
// usually running), then yields to any thread on its core (a lower-priority holder there gets to finish), then naps
// 20 us at a time. The key is read once, before the first lock (kInitOnly), so every lock and unlock agree.
class RecordMutex {
public:
  void lock() {
    if (!Spin()) {
      mutex_.lock();
      return;
    }
    uint32_t expected = 0;
    if (!flag_.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed)) [[unlikely]]
      LockSlow();
  }
  void unlock() {
    if (!Spin()) {
      mutex_.unlock();
      return;
    }
    flag_.store(0, std::memory_order_release);
  }
  // The slow paths and their waits since the last report (RecordLockReport).
  std::atomic<uint64_t> slow{0}, yields{0}, naps{0};

private:
  static bool Spin() {
    static const bool spin = REXCVAR_GET(blur_renut_record_spin);
    return spin;
  }
  void LockSlow() {
    slow.fetch_add(1, std::memory_order_relaxed);
    for (uint32_t i = 0;; ++i) {
      if (flag_.load(std::memory_order_relaxed) == 0) {
        uint32_t expected = 0;
        if (flag_.compare_exchange_weak(expected, 1, std::memory_order_acquire, std::memory_order_relaxed))
          return;
      }
      if (i < 64) {
#if defined(__aarch64__)
        __asm__ __volatile__("yield");
#endif
      } else if (i < 80) {
        yields.fetch_add(1, std::memory_order_relaxed);
#if defined(__SWITCH__)
        svcSleepThread(-2);  // YieldType_ToAnyThread: lower-priority threads on this core run too
#else
        std::this_thread::yield();
#endif
      } else {
        naps.fetch_add(1, std::memory_order_relaxed);
#if defined(__SWITCH__)
        svcSleepThread(20'000);
#else
        std::this_thread::sleep_for(std::chrono::microseconds(20));
#endif
      }
    }
  }
  std::atomic<uint32_t> flag_{0};
  std::mutex mutex_;
};

// Guards the three below; never taken with State().mutex (the render thread
// replays captured buffers without it). A buffer a queued run still holds is
// replaced, not cleared, when the game records it again (BeginRecording).
RecordMutex g_record_mutex;
std::unordered_map<u32, std::shared_ptr<HostCommandBuffer>> g_buffers;

// Buffers no recording or queued run holds any more, kept with their capacity:
// a buffer recorded again is a fresh one while a run holds the old, and
// growing a fresh arena to a frame's worth of snapshots cost the recording
// threads more than recording. Their own lock: the last reference may drop
// with g_record_mutex held (BeginRecording) or on the render thread.
// Bounded by count and by the capacity they keep: one frame's worth of
// recordings is what is worth keeping, and an arena one unusual frame grew
// past kMaxKeptArena goes rather than holding that memory for good.
constexpr size_t kMaxFreeBuffers = 256;
// banjo-nx (2026-10-28, the d4 console crash): 32 MB (was 64): the heap was all but full (see GrowArena).
constexpr size_t kMaxFreeBytes = 32u << 20;
constexpr size_t kMaxKeptArena = 8u << 20;
std::mutex g_free_mutex;
std::vector<std::unique_ptr<HostCommandBuffer>> g_free_buffers;
size_t g_free_bytes = 0;  // arena and op capacity of g_free_buffers
std::atomic<size_t> g_live_buffers{0};

size_t CapacityBytes(const HostCommandBuffer &buffer) {
  return buffer.arena.capacity() + buffer.ops.capacity() * sizeof(buffer.ops[0]);
}

// banjo-nx (2026-10-28, the d4 console crash: std::bad_alloc in CopyState's arena growth on a recording thread, the heap
// 2.9 of its 3.08 GB in use with 18 MB of it free in 243,000 pieces). The spare buffers given back to the heap (when an
// arena's growth found no room).
void ReleaseSpareBuffers() {
  std::vector<std::unique_ptr<HostCommandBuffer>> spare;
  {
    std::lock_guard lock(g_free_mutex);
    spare.swap(g_free_buffers);
    g_free_bytes = 0;
  }
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
    RGPU_WARN("command buffers: an arena's growth found no room in the heap: {} spare buffers given back, asked again",
              spare.size());
}
// An arena grows by doubling while small and by a quarter past 8 MB (doubling a 30 MB arena asks for 60 MB in one
// piece, beside the 30 it copies from); when that allocation fails, the spare buffers go back to the heap and it is
// asked again for no more than the arena needs (and only then does the failure stand).
constexpr size_t kArenaQuarterFrom = 8u << 20;
template <typename Arena>
void ReserveArena(Arena &arena, size_t size) {
  const size_t cap = arena.capacity();
  if (size > cap) {
    const size_t want = cap < kArenaQuarterFrom ? std::max(size, cap * 2) : std::max(size, cap + cap / 4);
    try {
      arena.reserve(want);
    } catch (const std::bad_alloc &) {
      ReleaseSpareBuffers();
#if defined(PLUME_DEKO3D)
      // sasrt-nx (2026-11-09; port 58; SASRT's docked crash: this growth found no room with 500-700 MB free in the GPU
      // region): the region's free bottom back to the heap first (plume_deko3d.h, Deko3DRegionGiveBack).
      plume::Deko3DRegionGiveBack();
#endif
      arena.reserve(size);
    }
  }
}
template <typename Arena>
void GrowArena(Arena &arena, size_t size) {
  ReserveArena(arena, size);
  arena.resize(size);
}
// A vector emptied for a new recording: one whose capacity is past 'keep' bytes and more than twice what its last
// recording used is given back to the heap (a buffer kept a loading screen's or a busy frame's size for good, and the
// d4 run's 36 buffers held 75 MB, its spares 42 MB more).
template <typename V>
void ResetForRecording(V &v, size_t keep) {
  const size_t cap_bytes = v.capacity() * sizeof(typename V::value_type);
  // sasrt-nx (2026-11-09, sasrt_memory_governor; port 58): while memory is low, a quarter of 'keep' and a quarter more
  // than the last recording used are enough to give the rest back (SASRT's docked crash: this arena could not grow)
  if (banjo::renut_gpu::MemoryLow() && cap_bytes > keep / 4 && v.capacity() > v.size() + v.size() / 4) {
    V().swap(v);
  } else if (cap_bytes > keep && v.capacity() / 2 > v.size()) {
    V().swap(v);
  } else {
    v.clear();
  }
}

std::shared_ptr<HostCommandBuffer> NewBuffer() {
  std::unique_ptr<HostCommandBuffer> buffer;
  {
    std::lock_guard lock(g_free_mutex);
    if (!g_free_buffers.empty()) {
      buffer = std::move(g_free_buffers.back());
      g_free_buffers.pop_back();
      g_free_bytes -= CapacityBytes(*buffer);
    }
  }
  g_live_buffers.fetch_add(1, std::memory_order_relaxed);
  const uint64_t recording_id = g_recording_ids.fetch_add(1, std::memory_order_relaxed) + 1;
  if (buffer) {
    ResetForRecording(buffer->ops, kMaxKeptArena / 2);  // banjo-nx (2026-10-28)
    ResetForRecording(buffer->arena, kMaxKeptArena);
    buffer->ClearSortPlan();  // banjo-nx (2026-10-27)
  } else {
    buffer = std::make_unique<HostCommandBuffer>();
  }
  buffer->id = recording_id;
  return std::shared_ptr<HostCommandBuffer>(buffer.release(), [](HostCommandBuffer *retired) {
    g_live_buffers.fetch_sub(1, std::memory_order_relaxed);
    const size_t bytes = CapacityBytes(*retired);
    {
      std::lock_guard lock(g_free_mutex);
      if (g_free_buffers.size() < kMaxFreeBuffers && retired->arena.capacity() <= kMaxKeptArena &&
          g_free_bytes + bytes <= kMaxFreeBytes) {
        g_free_buffers.emplace_back(retired);
        g_free_bytes += bytes;
        return;
      }
    }
    delete retired;
  });
}
std::unordered_map<u32, DeviceRecording> g_recording_devices;
std::vector<PendingVertices> g_pending_vertices;

// banjo-nx (2026-10-19, CPU proposal 3, banjo_renut_cpu_memos): NewOp's lookups remembered per recording thread, so an
// op takes g_record_mutex only when something changed. g_record_generation is bumped (under the mutex) whenever a
// buffer is made, replaced or released; a DeviceRecording never moves (map nodes, never erased). g_pending_count
// mirrors g_pending_vertices.size(): with anything pending, NewOp takes the lock to copy it in first.
std::atomic<uint64_t> g_record_generation{1};
std::atomic<size_t> g_pending_count{0};
// blur-nx (2026-11-17, blur_renut_record_memo_local): a generation per command buffer (hashed: a collision only costs an
// extra lock), bumped with the global one wherever a buffer is made, begun, replaced or released, so a recording
// thread's memo is invalidated by changes to its own buffer only. As safe as the global check: a buffer is recorded on
// one thread at a time and begun or released only when nobody records into it (the comment above).
constexpr u32 kCbGenerations = 4096;
std::atomic<u32> g_cb_generation[kCbGenerations];
inline std::atomic<u32> &CbGeneration(u32 command_buffer) {
  return g_cb_generation[(command_buffer * 2654435761u) >> 20];
}
// blur-nx (2026-11-17, blur_renut_flush_fast_local): g_pending_vertices' entries per device, hashed (a collision only
// costs a lock), changed with the list under g_record_mutex; FlushRecordedVertices reads its own device's without it.
std::atomic<u32> g_pending_by_device[64];
inline std::atomic<u32> &PendingOf(u32 device_va) { return g_pending_by_device[(device_va * 2654435761u) >> 26]; }
inline void BumpRecordGeneration(u32 command_buffer) {
  g_record_generation.fetch_add(1, std::memory_order_acq_rel);
  CbGeneration(command_buffer).fetch_add(1, std::memory_order_acq_rel);
}
struct RecordCache {
  u32 device_va = 0, command_buffer = 0;
  uint64_t generation = 0;
  u32 cb_generation = 0;  // blur-nx (2026-11-17)
  HostCommandBuffer *buffer = nullptr;
  DeviceRecording *set = nullptr;
  // blur-nx (2026-11-17): NewOp's census (RecordLockReport): answered here, and the misses by why (another device or
  // buffer, the buffer begun / made / released since, this device's Begin*Vertices data pending).
  uint64_t hits = 0, miss_other = 0, miss_generation = 0, miss_pending = 0;
};
thread_local RecordCache t_record_cache;
// banjo-nx (2026-10-27, render thread night): the memo kept per recording device, by its g_touch slot (a device records
// on one thread at a time; its own buffer is only that thread's until it runs), for the devices that have one: a
// thread_local is a call on the Switch (__aarch64_read_tp), at every op. The same key and the same checks.
struct alignas(64) SlotRecordCache {
  RecordCache c;
};
SlotRecordCache g_record_cache_slots[native::kTouchSlots];

// sasrt-nx (2026-11-12, batch C/D, item C11; port 93): a changed count only (the sole writer holds g_record_mutex):
// the line every op's memo check reads is not written at each op with the same value. A reader that sees the count
// unchanged needs nothing the store would have published (0: it takes no pending data; not 0: it takes the mutex).
void SyncPendingCount() {
  const size_t n = g_pending_vertices.size();
  if (g_pending_count.load(std::memory_order_relaxed) != n)
    g_pending_count.store(n, std::memory_order_release);
}

u32 RecordingBuffer(u32 device_va) {
  const auto *word = GuestPtr<const be_u32>(device_va + d3d::kDeviceOffsetCommandBuffer);
  return word ? u32(*word) : 0u;
}

size_t Reserve(HostCommandBuffer &buffer, size_t size) {
  const size_t offset = (buffer.arena.size() + 15) & ~size_t(15);
  GrowArena(buffer.arena, offset + size);
  std::memset(buffer.arena.data() + offset, 0, size);  // banjo-nx (2026-10-27): as the zeroing resize gave them
  return offset;
}

// Appends rather than going through Reserve: resize would zero the bytes the
// copy then overwrites (a device snapshot per op).
size_t CopyIn(HostCommandBuffer &buffer, const u8 *data, size_t size) {
  ReserveArena(buffer.arena, ((buffer.arena.size() + 15) & ~size_t(15)) + size);  // banjo-nx (2026-10-28)
  buffer.arena.resize((buffer.arena.size() + 15) & ~size_t(15));
  const size_t offset = buffer.arena.size();
  buffer.arena.insert(buffer.arena.end(), data, data + size);
  return offset;
}

// Whether two whole chunks differ: fixed-size loads the compiler keeps inline
// (and vectorizes), where a memcmp call per chunk cost more than the bytes.
bool ChunkDiffers(const u8 *a, const u8 *b) {
#if defined(__aarch64__)
  // banjo-nx (2026-10-23, batch 5): four 16-byte loads a side, one OR tree, one test (the recording threads' NewOp
  // runs this for every chunk of every op: the largest host cost of the game threads).
  const uint8x16_t d0 = veorq_u8(vld1q_u8(a), vld1q_u8(b)), d1 = veorq_u8(vld1q_u8(a + 16), vld1q_u8(b + 16));
  const uint8x16_t d2 = veorq_u8(vld1q_u8(a + 32), vld1q_u8(b + 32)), d3 = veorq_u8(vld1q_u8(a + 48), vld1q_u8(b + 48));
  const uint64x2_t o = vreinterpretq_u64_u8(vorrq_u8(vorrq_u8(d0, d1), vorrq_u8(d2, d3)));
  return (vgetq_lane_u64(o, 0) | vgetq_lane_u64(o, 1)) != 0;
#endif
  uint64_t diff = 0;
  for (u32 k = 0; k < kChunkBytes; k += 8) {
    uint64_t x, y;
    std::memcpy(&x, a + k, 8);
    std::memcpy(&y, b + k, 8);
    diff |= x ^ y;
  }
  return diff != 0;
}

#if defined(__aarch64__)
// banjo-nx (2026-10-23, batch 6b): whether any of four consecutive whole chunks from 'a' differs from 'b', as bits 0-3:
// all sixteen loads of each side issued before any test (the recording threads' NewOp waited on each chunk's loads in
// turn: its compare line was 6 % of each recording thread), no branch per chunk.
inline uint64_t FourChunksDiffer(const u8 *a, const u8 *b) {
  uint64_t bits = 0;
  uint8x16_t o[4];
  for (u32 k = 0; k < 4; ++k) {
    const u8 *x = a + k * kChunkBytes, *y = b + k * kChunkBytes;
    const uint8x16_t d0 = veorq_u8(vld1q_u8(x), vld1q_u8(y)), d1 = veorq_u8(vld1q_u8(x + 16), vld1q_u8(y + 16));
    const uint8x16_t d2 = veorq_u8(vld1q_u8(x + 32), vld1q_u8(y + 32)), d3 = veorq_u8(vld1q_u8(x + 48), vld1q_u8(y + 48));
    o[k] = vorrq_u8(vorrq_u8(d0, d1), vorrq_u8(d2, d3));
  }
  for (u32 k = 0; k < 4; ++k)
    bits |= uint64_t(vmaxvq_u8(o[k]) != 0) << k;
  return bits;
}
#endif

// The chunks of 'device' that differ from 'shadow', which takes them: a chunk at a time.
uint64_t ChangedChunksReference(const u8 *device, u8 *shadow) {
  uint64_t changed = 0;
  for (u32 i = 0; i < kPlainChunks; ++i) {
    const u32 at = ChunkOffset(i), size = ChunkSize(i);
    if (std::memcmp(device + at, shadow + at, size) != 0) {
      std::memcpy(shadow + at, device + at, size);
      changed |= uint64_t(1) << i;
    }
  }
  return changed;
}

// banjo-nx (2026-10-26): the chunks of 'device' that differ from 'shadow' (ChangedChunks' compare; the shadow is left).
uint64_t DifferingChunks(const u8 *device, const u8 *shadow) {
#if defined(__aarch64__)
  // banjo-nx (2026-10-23, batch 6b): the fetch constants' chunks and the tail's whole ones four at a time (the two
  // regions are each contiguous), the tail's last (short) chunk on its own: the same mask as the loop below.
  uint64_t changed = 0;
  u32 i = 0;
  for (; i + 4 <= kFetchChunks; i += 4)
    changed |= FourChunksDiffer(device + kFetchBegin + i * kChunkBytes, shadow + kFetchBegin + i * kChunkBytes) << i;
  for (; i < kFetchChunks; ++i)
    changed |= uint64_t(ChunkDiffers(device + kFetchBegin + i * kChunkBytes, shadow + kFetchBegin + i * kChunkBytes))
               << i;
  constexpr u32 kTailWhole = kLastChunkBytes == kChunkBytes ? kTailChunks : kTailChunks - 1;
  u32 t = 0;
  for (; t + 4 <= kTailWhole; t += 4)
    changed |= FourChunksDiffer(device + kTailBegin + t * kChunkBytes, shadow + kTailBegin + t * kChunkBytes)
               << (kFetchChunks + t);
  for (; t < kTailWhole; ++t)
    changed |= uint64_t(ChunkDiffers(device + kTailBegin + t * kChunkBytes, shadow + kTailBegin + t * kChunkBytes))
               << (kFetchChunks + t);
  if constexpr (kTailWhole < kTailChunks) {
    const u32 at = kTailBegin + kTailWhole * kChunkBytes;
    changed |= uint64_t(std::memcmp(device + at, shadow + at, kLastChunkBytes) != 0) << (kFetchChunks + kTailWhole);
  }
  return changed;
#else
  uint64_t changed = 0;
  for (u32 i = 0; i < kPlainChunks; ++i) {
    const u32 at = ChunkOffset(i), size = ChunkSize(i);
    // banjo-nx (2026-10-23, batch 6): the last chunk compared at its fixed size too (inline, no C library call at
    // every op).
    const bool differs = size == kChunkBytes ? ChunkDiffers(device + at, shadow + at)
                                             : std::memcmp(device + at, shadow + at, kLastChunkBytes) != 0;
    changed |= uint64_t(differs) << i;
  }
  return changed;
#endif
}

// banjo-nx (2026-10-26): the shadow takes the device's 'chunks'.
void CopyChunks(const u8 *device, u8 *shadow, uint64_t chunks) {
  for (; chunks; chunks &= chunks - 1) {
    const u32 c = u32(std::countr_zero(chunks));
    const u32 at = ChunkOffset(c);
    // banjo-nx (2026-10-23): a whole chunk as a fixed-size copy (inline), not a C library call per changed chunk.
    if (ChunkSize(c) == kChunkBytes)
      std::memcpy(shadow + at, device + at, kChunkBytes);
    else
      std::memcpy(shadow + at, device + at, kLastChunkBytes);
  }
}

// The same (ChangedChunks below).
uint64_t ChangedChunks(const u8 *device, u8 *shadow) {
  const uint64_t changed = DifferingChunks(device, shadow);
  CopyChunks(device, shadow, changed);
  return changed;
}

// banjo-nx (2026-10-26, banjo_renut_tags): ChangedChunks for the chunks in 'marked' only, the shadow's others being the
// device's. Each marked chunk is compared and stored into the shadow at once (the device's bytes as loaded for the
// compare, stored whether or not they differ: no branch on the compare; storing equal bytes changes nothing).
// Title update 3 (banjo-nx 2026-10-29): the snapshot is 12,720 bytes (d3d.h), its last chunk 48 bytes (40 on the base).
// katamari-nx (2026-10-30): XDK 2.0.5632's snapshot is 12,704 bytes (0x31A0): its last chunk is 32 bytes.
// renut-ultimate (2026-11-20, port 196): TFU-nx (2026-11-11): XDK 2.0.6534's is 12,712 bytes (0x31A8, as N&B's base):
// 40 bytes. S4ep1-nx (2026-11-14): XDK 2.0.11626's is 12,888 bytes (0x3258): 24 bytes, one vector and one word.
// TakeLastChunk takes any of the three (a port at another XDK changes d3d.h's snapshot size, not this).
static_assert(kLastChunkBytes == 24 || kLastChunkBytes == 32 || kLastChunkBytes == 40, "TakeLastChunk's loads");
inline uint64_t TakeChunk(const u8 *device, u8 *shadow) {
#if defined(__aarch64__)
  const uint8x16_t a0 = vld1q_u8(device), a1 = vld1q_u8(device + 16);
  const uint8x16_t a2 = vld1q_u8(device + 32), a3 = vld1q_u8(device + 48);
  const uint8x16_t d0 = veorq_u8(a0, vld1q_u8(shadow)), d1 = veorq_u8(a1, vld1q_u8(shadow + 16));
  const uint8x16_t d2 = veorq_u8(a2, vld1q_u8(shadow + 32)), d3 = veorq_u8(a3, vld1q_u8(shadow + 48));
  vst1q_u8(shadow, a0);
  vst1q_u8(shadow + 16, a1);
  vst1q_u8(shadow + 32, a2);
  vst1q_u8(shadow + 48, a3);
  return vmaxvq_u8(vorrq_u8(vorrq_u8(d0, d1), vorrq_u8(d2, d3))) != 0;
#else
  const bool differs = ChunkDiffers(device, shadow);
  std::memcpy(shadow, device, kChunkBytes);
  return differs;
#endif
}
inline uint64_t TakeLastChunk(const u8 *device, u8 *shadow) {
#if defined(__aarch64__)
  if constexpr (kLastChunkBytes == 24) {
    // S4ep1-nx (port 196): 16 bytes as a vector, the last 8 as one word.
    const uint8x16_t a0 = vld1q_u8(device);
    const uint8x16_t d0 = veorq_u8(a0, vld1q_u8(shadow));
    vst1q_u8(shadow, a0);
    uint64_t x, y;
    std::memcpy(&x, device + 16, 8);
    std::memcpy(&y, shadow + 16, 8);
    std::memcpy(shadow + 16, &x, 8);
    return uint64_t((vmaxvq_u8(d0) != 0) | (x != y));
  } else {
    const uint8x16_t a0 = vld1q_u8(device), a1 = vld1q_u8(device + 16);
    const uint8x16_t d0 = veorq_u8(a0, vld1q_u8(shadow)), d1 = veorq_u8(a1, vld1q_u8(shadow + 16));
    vst1q_u8(shadow, a0);
    vst1q_u8(shadow + 16, a1);
    if constexpr (kLastChunkBytes == 40) {
      // TFU-nx (port 196): the last 8 bytes as one word.
      uint64_t x, y;
      std::memcpy(&x, device + 32, 8);
      std::memcpy(&y, shadow + 32, 8);
      std::memcpy(shadow + 32, &x, 8);
      return uint64_t((vmaxvq_u8(vorrq_u8(d0, d1)) != 0) | (x != y));
    }
    return vmaxvq_u8(vorrq_u8(d0, d1)) != 0;
  }
#else
  const bool differs = std::memcmp(device, shadow, kLastChunkBytes) != 0;
  std::memcpy(shadow, device, kLastChunkBytes);
  return differs;
#endif
}
uint64_t ChangedChunksMarked(const u8 *device, u8 *shadow, uint64_t marked) {
  constexpr u32 kLast = kPlainChunks - 1;
  constexpr u32 kLastAt = kTailBegin + (kTailChunks - 1) * kChunkBytes;
  uint64_t changed = 0;
  if ((marked >> kLast) & 1)
    changed = TakeLastChunk(device + kLastAt, shadow + kLastAt) << kLast;
  for (marked &= ~(uint64_t(1) << kLast); marked; marked &= marked - 1) {
    const u32 c = u32(std::countr_zero(marked));
    // ChunkOffset without a branch: the tail's chunks follow the fetch constants' 8 KB further on.
    const u32 at = kFetchBegin + c * kChunkBytes + (c >= kFetchChunks ? kTailBegin - kFetchBegin - kFetchBytes : 0u);
    changed |= TakeChunk(device + at, shadow + at) << c;
  }
  return changed;
}
static_assert(kPlainChunks - 1 == kFetchChunks + kTailChunks - 1 && kLastChunkBytes < kChunkBytes);

// banjo-nx (2026-10-26, Unleashed-style step 1): each chunk's fields, word by word, and the chunks holding more than
// one field (whose changed words are looked at: the targets share one with the index buffer and the streams).
struct PlainFieldTables {
  u32 chunk[64] = {};
  u32 word[64][16] = {};
  uint64_t mixed = 0;
};
constexpr PlainFieldTables MakePlainFieldTables() {
  PlainFieldTables t{};
  for (u32 c = 0; c < kPlainChunks; ++c) {
    const u32 at = ChunkOffset(c), words = ChunkSize(c) / 4;
    u32 first = 0;
    for (u32 w = 0; w < words; ++w) {
      const u32 f = FieldOfOffset(at + 4 * w);
      t.word[c][w] = f;
      t.chunk[c] |= f;
      if (w == 0)
        first = f;
      else if (f != first)
        t.mixed |= uint64_t(1) << c;
    }
  }
  return t;
}
constexpr PlainFieldTables kPlainFields = MakePlainFieldTables();
static_assert(kChunkBytes / 4 == 16 && kLastChunkBytes % 4 == 0);

// The words (bit w: bytes 4w-4w+3) of a chunk that differ.
inline u32 ChunkWordMask(const u8 *a, const u8 *b, u32 size) {
#if defined(__aarch64__)
  if (size == kChunkBytes) {
    const uint32x4_t d0 = veorq_u32(vld1q_u32(reinterpret_cast<const uint32_t *>(a)),
                                    vld1q_u32(reinterpret_cast<const uint32_t *>(b)));
    const uint32x4_t d1 = veorq_u32(vld1q_u32(reinterpret_cast<const uint32_t *>(a + 16)),
                                    vld1q_u32(reinterpret_cast<const uint32_t *>(b + 16)));
    const uint32x4_t d2 = veorq_u32(vld1q_u32(reinterpret_cast<const uint32_t *>(a + 32)),
                                    vld1q_u32(reinterpret_cast<const uint32_t *>(b + 32)));
    const uint32x4_t d3 = veorq_u32(vld1q_u32(reinterpret_cast<const uint32_t *>(a + 48)),
                                    vld1q_u32(reinterpret_cast<const uint32_t *>(b + 48)));
    // Each word's lane all ones when it differs, narrowed to a byte per word, weighted by its bit and summed.
    const uint16x8_t n01 = vcombine_u16(vmovn_u32(vtstq_u32(d0, d0)), vmovn_u32(vtstq_u32(d1, d1)));
    const uint16x8_t n23 = vcombine_u16(vmovn_u32(vtstq_u32(d2, d2)), vmovn_u32(vtstq_u32(d3, d3)));
    static const uint8_t kWeights[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    const uint8x16_t bits = vandq_u8(vcombine_u8(vmovn_u16(n01), vmovn_u16(n23)), vld1q_u8(kWeights));
    return u32(vaddv_u8(vget_low_u8(bits))) | u32(vaddv_u8(vget_high_u8(bits))) << 8;
  }
#endif
  u32 mask = 0;
  for (u32 w = 0; w * 4 < size; ++w) {
    u32 x, y;
    std::memcpy(&x, a + 4 * w, 4);
    std::memcpy(&y, b + 4 * w, 4);
    mask |= u32(x != y) << w;
  }
  return mask;
}

// The fields of the mixed chunks among 'chunks' whose words differ between 'device' and 'shadow' (before the shadow
// takes them), and of the other chunks in 'changed' as a whole (each holds one field).
u32 MixedChunkFields(const u8 *device, const u8 *shadow, uint64_t chunks) {
  u32 fields = 0;
  for (uint64_t left = chunks & kPlainFields.mixed; left; left &= left - 1) {
    const u32 c = u32(std::countr_zero(left));
    const u32 at = ChunkOffset(c);
    for (u32 words = ChunkWordMask(device + at, shadow + at, ChunkSize(c)); words; words &= words - 1)
      fields |= kPlainFields.word[c][std::countr_zero(words)];
  }
  return fields;
}
u32 WholeChunkFields(uint64_t chunks) {
  u32 fields = 0;
  for (; chunks; chunks &= chunks - 1)
    fields |= kPlainFields.chunk[std::countr_zero(chunks)];
  return fields;
}
static_assert(native::kTouchFetchBegin == kFetchBegin && native::kTouchFetchEnd == kFetchBegin + kFetchBytes &&
              native::kTouchTailBegin == kTailBegin && native::TouchBit(kFetchBegin) == 1 &&
              native::TouchBit(kTailBegin) == uint64_t(1) << kFetchChunks &&
              native::TouchBit(d3d::kDeviceSnapshotSize - 1) == uint64_t(1) << (kPlainChunks - 1),
              "the setters' marks are the ops' chunks");

// banjo-nx (2026-10-26, banjo_renut_tags): the marks' counts for the 10 s line, kept per recording device
// (DeviceRecording::counts: no thread_local, a call on the Switch) and added to the shared ones every kTagFlushOps ops.
constexpr u32 kTagFlushOps = 1024;
std::atomic<uint64_t> g_tag_ops{0}, g_tag_marked_ops{0}, g_tag_marked{0}, g_tag_changed{0}, g_tag_checks{0};
std::atomic<uint64_t> g_tag_unmarked{0};
std::atomic<bool> g_tags_failed{false};
std::atomic<uint64_t> g_tag_logged_chunks{0};

void FlushTagCounts(TagCounts &t) {
  g_tag_ops.fetch_add(t.ops, std::memory_order_relaxed);
  g_tag_marked_ops.fetch_add(t.marked_ops, std::memory_order_relaxed);
  g_tag_marked.fetch_add(t.marked, std::memory_order_relaxed);
  g_tag_changed.fetch_add(t.changed, std::memory_order_relaxed);
  g_tag_checks.fetch_add(t.checks, std::memory_order_relaxed);
  g_census_setter_changed.fetch_add(t.setter_changed, std::memory_order_relaxed);
  g_census_inline_changed.fetch_add(t.inline_changed, std::memory_order_relaxed);
  g_census_touch_marks.fetch_add(t.touch_marks, std::memory_order_relaxed);
  g_census_tag_only_marks.fetch_add(t.tag_only_marks, std::memory_order_relaxed);
  g_census_ops.fetch_add(t.census_ops, std::memory_order_relaxed);
  for (u32 b = 0; b < 18; ++b) {
    if (t.setter_fields[b])
      g_census_setter_fields[b].fetch_add(t.setter_fields[b], std::memory_order_relaxed);
    if (t.inline_fields[b])
      g_census_inline_fields[b].fetch_add(t.inline_fields[b], std::memory_order_relaxed);
  }
  t = {};
}

// Chunks a check found changed without a mark: each chunk's first such word logged once (its device offset names the
// field and so the writer the marks miss), counted; outside the verify form the marks are not used again.
void NoteUnmarked(const u8 *device, const u8 *shadow, uint64_t chunks, OpKind kind, bool verify,
                  const uint64_t tags[3]) {
  g_tag_unmarked.fetch_add(uint64_t(std::popcount(chunks)), std::memory_order_relaxed);
  const uint64_t fresh = chunks & ~g_tag_logged_chunks.fetch_or(chunks, std::memory_order_relaxed);
  for (uint64_t left = fresh; left; left &= left - 1) {
    const u32 c = u32(std::countr_zero(left));
    const u32 at = ChunkOffset(c), size = ChunkSize(c);
    u32 word = at;
    while (word + 4 <= at + size && std::memcmp(device + word, shadow + word, 4) == 0)
      word += 4;
    u32 now = 0, was = 0;
    if (word + 4 <= at + size) {
      std::memcpy(&now, device + word, 4);
      std::memcpy(&was, shadow + word, 4);
    }
    RGPU_WARN("Recording from marks: device +0x{:X} (chunk {}, +0x{:X}-0x{:X}) changed without a mark before a {} "
              "op: 0x{:08X}, was 0x{:08X}; tags {:016X} {:016X} {:016X} (banjo_renut_tags_check)",
              word, c, at, at + size - 1,
              kind == OpKind::Draw ? "draw" : kind == OpKind::Clear ? "clear" : kind == OpKind::Resolve ? "resolve" : "run",
              __builtin_bswap32(now), __builtin_bswap32(was), tags[0], tags[1], tags[2]);
  }
  // banjo-nx (2026-10-26, setter rewrite): with the setters' tags kept apart, a write they covered by a tag alone
  // could be the one missed: their tags go back into the devices first, and only a miss after that stops the marks.
  if (!verify && native::g_tags_apart) {
    native::g_tags_apart = false;
    RGPU_ERROR("Recording from marks: a device chunk changed without a mark; the setters' tags go into the devices again "
               "from now on (banjo_renut_tags_apart)");
    return;
  }
  if (!verify && !g_tags_failed.exchange(true, std::memory_order_relaxed))
    RGPU_ERROR("Recording from marks: a device chunk changed without a mark; every op compares every chunk from now on "
               "(banjo_renut_tags)");
}

// banjo-nx (2026-10-23, batch 6b): ChangedChunks against ChangedChunksReference once, at the first op, on made-up
// devices: each chunk alone changed (one byte at a varying place), then every chunk; the masks and the shadows must be
// the same. A difference is logged (and would make every op keep every chunk: the slow, safe form).
bool ChangedChunksSelfTest() {
  std::vector<u8> device(d3d::kDeviceSnapshotSize), a(d3d::kDeviceSnapshotSize), b(d3d::kDeviceSnapshotSize);
  uint32_t seed = 0x9E3779B9u;
  for (u8 &x : device) {
    seed = seed * 1664525u + 1013904223u;
    x = u8(seed >> 24);
  }
  bool ok = true;
  for (u32 c = 0; c <= kPlainChunks && ok; ++c) {
    a = device;
    b = device;
    std::vector<u8> changed = device;
    for (u32 i = 0; i < kPlainChunks; ++i) {
      if (c < kPlainChunks && i != c)
        continue;
      const u32 at = ChunkOffset(i) + (i * 7u + c) % ChunkSize(i);
      changed[at] ^= 0x5Au;
    }
    const uint64_t fast = ChangedChunks(changed.data(), a.data());
    const uint64_t reference = ChangedChunksReference(changed.data(), b.data());
    ok = fast == reference && a == b;
  }
  if (!ok)
    RGPU_ERROR("ChangedChunks differs from its reference form: every op keeps every chunk (batch 6b self-test)");
  return ok;
}

// An op's state (Op::state) from the device.
size_t CopyState(HostCommandBuffer &buffer, const u8 *device, uint64_t plain_dirty,
                 uint64_t vs_dirty, uint64_t ps_dirty) {
  auto &arena = buffer.arena;
  const size_t offset = (arena.size() + 15) & ~size_t(15);
  // banjo-nx (2026-10-23, the recording threads' NewOp): the op's bytes counted first and the arena grown once, then
  // fixed-size copies (inline) for whole chunks and constant groups, instead of a vector insert (a capacity test and a
  // C library copy) for each chunk and each run of groups. The same bytes in the same order.
  size_t bytes = 0;
  for (uint64_t chunks = plain_dirty; chunks; chunks &= chunks - 1)
    bytes += ChunkSize(u32(std::countr_zero(chunks)));
  bytes += size_t(std::popcount(vs_dirty) + std::popcount(ps_dirty)) * kConstantGroupBytes;
  GrowArena(arena, offset + bytes);
  u8 *to = arena.data() + offset;
  for (uint64_t chunks = plain_dirty; chunks; chunks &= chunks - 1) {
    const u32 i = u32(std::countr_zero(chunks));
    const u8 *from = device + ChunkOffset(i);
    const u32 size = ChunkSize(i);
    if (size == kChunkBytes)
      std::memcpy(to, from, kChunkBytes);
    else
      std::memcpy(to, from, kLastChunkBytes);  // banjo-nx (2026-10-23, batch 6): the only other size
    to += size;
  }
  for (const auto [dirty, base] : {std::pair{vs_dirty, kVertexShaderF}, std::pair{ps_dirty, kPixelShaderF}}) {
    ForEachGroupRun(dirty, [&](u32 first, u32 count) {
      const u8 *from = device + base + first * kConstantGroupBytes;
      for (u32 k = 0; k < count; ++k)
        std::memcpy(to + k * kConstantGroupBytes, from + k * kConstantGroupBytes, kConstantGroupBytes);
      to += count * kConstantGroupBytes;
    });
  }
  return offset;
}

// Brings 'state', the previous op's device (or anything, before a buffer's
// first op), to the op's.
// 'groups' (banjo_renut_replay_group_pointers): the replay's 128 group pointers, set to the op's recorded groups instead
// of copying them into the image; null: copied, as before.
// halo3-nx (2026-11-03, banjo_renut_replay_inherits): on the console a command buffer holds the state set in it and every
// other register stays as the running device left it. BeginCommandBuffer's inherit tags say so: Halo 3 passes all ones but
// five bits, and records on a no-hardware device of its own, whose targets are 0 and whose depth, blend and viewport are
// defaults. A recording's first op keeps every chunk, so replaying it wrote those over the running device: 75 % of the
// draws of the first mission had no targets and were dropped, without a word. Now: the words that differ from the device
// at Begin are the recording's, the others come from the running device. Float constants are apart: vs_set, ps_set.
constexpr u32 kInheritTailBegin = u32(offsetof(d3d::Device, m_Constants)) + 0x2300;  // 0x2780, after both float blocks
static_assert(kInheritTailBegin == 0x2780);

// blur-nx (2026-11-11, CPU batch 2): the words that differ from the device at Begin, 128 bytes (one set_words word) at a
// time. The console race profile of the LTO+PGO build had this scan, inlined into NewOp, at about 5 % of a core on the
// job threads (886 four-byte compares and branches an op, every op). On the AArch64 each 16 bytes is one compare of four
// words; a block with no difference costs eight compares and a test; a differing block's bits are the same bits the word
// loop sets. BLUR_SET_WORDS_SCALAR (compile time): the word loop.
#if defined(__aarch64__) && !defined(BLUR_SET_WORDS_SCALAR)
inline u32 DifferingWordsOf128(const u8 *a, const u8 *b) {
  u32 mask = 0;
  for (u32 q = 0; q < 8; ++q) {
    const uint32x4_t ne = vmvnq_u32(vceqq_u32(vreinterpretq_u32_u8(vld1q_u8(a + q * 16)),
                                              vreinterpretq_u32_u8(vld1q_u8(b + q * 16))));
    // Lane k (bytes 4k..4k+3 of this quarter) is word 4q + k of the block: its bit is 1 << (4q + k).
    const uint32x4_t bits = vandq_u32(ne, uint32x4_t{1u, 2u, 4u, 8u});
    mask |= vaddvq_u32(bits) << (4 * q);
  }
  return mask;
}
inline bool Equal128(const u8 *a, const u8 *b) {
  uint32x4_t x = vdupq_n_u32(0);
  for (u32 q = 0; q < 8; ++q)
    x = vorrq_u32(x, veorq_u32(vreinterpretq_u32_u8(vld1q_u8(a + q * 16)),
                               vreinterpretq_u32_u8(vld1q_u8(b + q * 16))));
  return vmaxvq_u32(x) == 0;
}
#endif

#if defined(__aarch64__) && !defined(BLUR_SET_WORDS_SCALAR)
// The two NEON helpers against the word compare, once, on the device (the PC build has no NEON path for the guard to
// compare): 128-byte blocks with no difference, one, several and every word different, each byte of a word in turn.
// A failure logs an error and every op keeps the word loop.
bool SetWordsSelfTest() {
  alignas(16) u8 a[128], b[128];
  uint32_t seed = 0x2545F491u;
  bool ok = true;
  for (u32 round = 0; round < 64 && ok; ++round) {
    for (u32 i = 0; i < 128; ++i) {
      seed = seed * 1664525u + 1013904223u;
      a[i] = b[i] = u8(seed >> 24);
    }
    const u32 flips = round < 32 ? round : 32 + (round % 7) * 9;  // 0..31 single words, then many
    for (u32 f = 0; f < flips; ++f) {
      seed = seed * 1664525u + 1013904223u;
      const u32 at = round < 32 ? round * 4 + (round & 3) : (seed >> 9) % 128;
      b[at] ^= u8(1u + ((seed >> 3) & 0x7F));
    }
    u32 want = 0;
    for (u32 w = 0; w < 32; ++w)
      want |= u32(std::memcmp(a + w * 4, b + w * 4, 4) != 0) << w;
    ok = DifferingWordsOf128(a, b) == want && Equal128(a, b) == (want == 0);
  }
  if (!ok)
    RGPU_ERROR("NoteSetWords' NEON compare differs from the word compare: every op keeps the word loop (blur-nx self-test)");
  return ok;
}
#endif

// blur-nx (2026-11-13): one 128-byte block's words that differ from the Begin image (NoteSetWords' scan, any platform).
inline void NoteSetBlock(HostCommandBuffer &buffer, const DeviceRecording &set, const u8 *device, u32 at) {
  if (at + 128 > d3d::kDeviceSnapshotSize) {  // the snapshot's short last block, word by word
    for (; at < d3d::kDeviceSnapshotSize; at += 4) {
      if (std::memcmp(device + at, set.begin_image + at, 4) != 0)
        buffer.set_words[at / 128] |= 1u << ((at / 4) & 31);
    }
    return;
  }
#if defined(__aarch64__) && !defined(BLUR_SET_WORDS_SCALAR)
  static const bool neon_ok = SetWordsSelfTest();
  if (neon_ok) {
    if (!Equal128(device + at, set.begin_image + at))
      buffer.set_words[at / 128] |= DifferingWordsOf128(device + at, set.begin_image + at);
    return;
  }
#endif
  for (u32 w = 0; w < 128; w += 4) {
    if (std::memcmp(device + at + w, set.begin_image + at + w, 4) != 0)
      buffer.set_words[at / 128] |= 1u << (((at + w) / 4) & 31);
  }
}

inline void NoteSetWords(HostCommandBuffer &buffer, const DeviceRecording &set, const u8 *device) {
  const auto scan_words = [&](u32 from, u32 to) {
    for (u32 at = from; at < to; at += 4) {
      if (std::memcmp(device + at, set.begin_image + at, 4) != 0)
        buffer.set_words[at / 128] |= 1u << ((at / 4) & 31);
    }
  };
#if defined(__aarch64__) && !defined(BLUR_SET_WORDS_SCALAR)
  static const bool neon_ok = SetWordsSelfTest();
  const auto scan = [&](u32 from, u32 to) {
    if (!neon_ok) {
      scan_words(from, to);
      return;
    }
    u32 at = from;
    for (; at + 128 <= to; at += 128) {  // 'from' is a multiple of 128 (static_asserts below): one set_words word a block
      if (buffer.set_words[at / 128] == 0xFFFFFFFFu)  // #218 (2026-11-17): all 32 words marked already
        continue;
      if (Equal128(device + at, set.begin_image + at))
        continue;
      buffer.set_words[at / 128] |= DifferingWordsOf128(device + at, set.begin_image + at);
    }
    scan_words(at, to);  // the tail of a range that does not end on 128 bytes
  };
  static_assert(kFetchBegin % 128 == 0 && kInheritTailBegin % 128 == 0, "NoteSetWords: blocks of 128 bytes");
#else
  const auto &scan = scan_words;
#endif
  scan(kFetchBegin, kVertexShaderF);
  scan(kInheritTailBegin, d3d::kDeviceSnapshotSize);
}

// blur-nx (2026-11-13, banjo_renut_set_words_changed): NoteSetWords for the 128-byte blocks holding a chunk of 'chunks'
// (the op's changed chunks; every chunk: the whole scan). The two ranges NoteSetWords scans are the plain chunks'.
static_assert(kInheritTailBegin == kTailBegin && kFetchBegin % 128 == 0 && kTailBegin % 128 == 0,
              "NoteSetWordsOf: NoteSetWords' ranges are the plain chunks, in 128-byte blocks");
std::atomic<uint64_t> g_set_words_checked{0}, g_set_words_missed{0};
void NoteSetWordsOf(HostCommandBuffer &buffer, const DeviceRecording &set, const u8 *device, uint64_t chunks) {
  static const bool on = REXCVAR_GET(banjo_renut_set_words_changed);
  static const bool verify = REXCVAR_GET(banjo_renut_set_words_verify);
  chunks &= kAllChunks;
  if (!on || chunks == kAllChunks) {
    NoteSetWords(buffer, set, device);
    return;
  }
  constexpr size_t kWords = sizeof(buffer.set_words) / sizeof(buffer.set_words[0]);
  u32 before[kWords];
  if (verify)
    std::memcpy(before, buffer.set_words, sizeof(before));
  uint64_t blocks[2] = {};
  for (uint64_t c = chunks; c; c &= c - 1) {
    const u32 b = ChunkOffset(u32(std::countr_zero(c))) / 128;
    blocks[b >> 6] |= uint64_t(1) << (b & 63);
  }
  for (u32 h = 0; h < 2; ++h) {
    for (uint64_t m = blocks[h]; m; m &= m - 1) {
      const u32 at = (h * 64 + u32(std::countr_zero(m))) * 128;
      // blur-nx (2026-11-17, #218 of docs/cpu-ideas-196-225-1116.md): a block whose 32 words are all marked since Begin
      // already has nothing to add (the marks only accumulate): its compare is skipped. Exact.
      if (buffer.set_words[at / 128] == 0xFFFFFFFFu && at + 128 <= d3d::kDeviceSnapshotSize)
        continue;
      NoteSetBlock(buffer, set, device, at);
    }
  }
  if (verify) {
    u32 mine[kWords];
    std::memcpy(mine, buffer.set_words, sizeof(mine));
    std::memcpy(buffer.set_words, before, sizeof(before));
    NoteSetWords(buffer, set, device);  // the whole scan's result is kept
    uint64_t missed = 0;
    for (size_t i = 0; i < kWords; ++i)
      missed += uint64_t(std::popcount(buffer.set_words[i] & ~mine[i]));
    g_set_words_checked.fetch_add(1, std::memory_order_relaxed);
    if (missed) {
      const uint64_t n = g_set_words_missed.fetch_add(missed, std::memory_order_relaxed);
      if (n < 8)
        RGPU_ERROR("banjo_renut_set_words_changed: {} words set since Begin outside the op's changed chunks", missed);
    }
  }
}

void BuildInheritRuns(HostCommandBuffer &buffer) {
  const auto is_set = [&buffer](u32 at) { return (buffer.set_words[at / 128] >> ((at / 4) & 31)) & 1; };
  const auto mark = [&buffer](u32 at) { buffer.set_words[at / 128] |= 1u << ((at / 4) & 31); };
  const auto any_set = [&is_set](u32 from, u32 to) {
    for (u32 at = from; at < to; at += 4) {
      if (is_set(at))
        return true;
    }
    return false;
  };
  const auto mark_all = [&mark](u32 from, u32 to) {
    for (u32 at = from; at < to; at += 4)
      mark(at);
  };
  // What the console writes as one: a word equal to the device's at Begin may still be the recording's (a zero in a
  // fetch constant, a stride as the last recording left it), and half of one draw's group with half of another's stretches
  // its geometry across the screen.
  // Each fetch constant's six words (its dirty tag), with its texture word.
  for (u32 n = 0; n < 32; ++n) {
    const u32 from = kFetchBegin + n * 24;
    const bool has_word = n < d3d::kTextureFetchSlots;
    const u32 word = has_word ? d3d::kDeviceOffsetTextures + n * 4 : 0;
    if (any_set(from, from + 24) || (has_word && is_set(word))) {
      mark_all(from, from + 24);
      if (has_word)
        mark(word);
    }
  }
  // The stream sources and their strides.
  if (any_set(d3d::kDeviceOffsetStreamSources, d3d::kDeviceOffsetTextures))
    mark_all(d3d::kDeviceOffsetStreamSources, d3d::kDeviceOffsetTextures);
  // A recording that binds a vertex shader decides all three: its declaration word stays 0 when D3DVertexShader_Bind gave
  // the shader one, and a null pixel shader looks like no change on a device that had none.
  if (is_set(d3d::kDeviceOffsetVertexShader)) {
    mark(d3d::kDeviceOffsetPixelShader);
    mark(d3d::kDeviceOffsetVertexDeclaration);
  }
  buffer.inherit_runs.clear();
  const auto add = [&](u32 from, u32 to) {
    for (u32 at = from; at < to;) {
      if (is_set(at)) {
        at += 4;
        continue;
      }
      const u32 start = at;
      while (at < to && !is_set(at))
        at += 4;
      buffer.inherit_runs.emplace_back(start, at - start);
    }
  };
  // blur-nx (2026-11-18, the Renut host batch's #13, blur_renut_inherit_runs_words): the same runs found a 32-bit word
  // of the bitmap at a time (a run of set words skipped by its count of ones, a run of unset ones by its zeros): the
  // loop above tested ~890 words one by one at every recording's first replay (0.18 ms a frame of the render thread).
  const auto add_words = [&](u32 from, u32 to) {
    const u32 b1 = to / 4;
    for (u32 b = from / 4; b < b1;) {
      const u32 bits = buffer.set_words[b >> 5] >> (b & 31);
      const u32 avail = std::min<u32>(32 - (b & 31), b1 - b);
      if (bits & 1) {
        b += std::min<u32>(u32(std::countr_one(bits)), avail);
        continue;
      }
      const u32 start = b;
      for (;;) {
        const u32 w = buffer.set_words[b >> 5] >> (b & 31);
        const u32 av = std::min<u32>(32 - (b & 31), b1 - b);
        const u32 z = std::min<u32>(w ? u32(std::countr_zero(w)) : 32u, av);
        b += z;
        if (z < av || b >= b1)
          break;
      }
      buffer.inherit_runs.emplace_back(start * 4, (b - start) * 4);
    }
  };
  static_assert(kFetchBegin % 4 == 0 && kVertexShaderF % 4 == 0 && kInheritTailBegin % 4 == 0 &&
                d3d::kDeviceSnapshotSize % 4 == 0);
  if (::renut::gpu::host_batch::On(::renut::gpu::host_batch::kInheritWords)) {
    add_words(kFetchBegin, kVertexShaderF);
    add_words(kInheritTailBegin, d3d::kDeviceSnapshotSize);
    if (::renut::gpu::host_batch::VerifyOn()) {  // blur-nx (2026-11-18): the runs the word loop made against the old loop's
      std::vector<std::pair<u32, u32>> words_way;
      words_way.swap(buffer.inherit_runs);
      add(kFetchBegin, kVertexShaderF);
      add(kInheritTailBegin, d3d::kDeviceSnapshotSize);
      if (words_way != buffer.inherit_runs)
        ::renut::gpu::host_batch::VerifyDiffer("#13: BuildInheritRuns' word runs differ from the old loop's");
    }
  } else {
    add(kFetchBegin, kVertexShaderF);
    add(kInheritTailBegin, d3d::kDeviceSnapshotSize);
  }
  // #3: the runs cut at the chunks' edges (a run lies in the fetch constants' chunks or in the tail's).
  buffer.inherit_chunk_runs.clear();
  buffer.inherit_bytes = 0;
  {
    u32 chunk = 0;
    buffer.inherit_chunk_at[0] = 0;
    for (const auto &[offset0, bytes0] : buffer.inherit_runs) {
      buffer.inherit_bytes += bytes0;
      for (u32 offset = offset0, left = bytes0; left;) {
        const u32 c = offset < kVertexShaderF ? (offset - kFetchBegin) / kChunkBytes
                                              : kFetchChunks + (offset - kTailBegin) / kChunkBytes;
        const u32 end = ChunkOffset(c) + ChunkSize(c);
        const u32 n = std::min<u32>(left, end - offset);
        while (chunk < c)
          buffer.inherit_chunk_at[++chunk] = uint16_t(buffer.inherit_chunk_runs.size());
        buffer.inherit_chunk_runs.emplace_back(offset, n);
        offset += n;
        left -= n;
      }
    }
    while (chunk < kPlainChunks)
      buffer.inherit_chunk_at[++chunk] = uint16_t(buffer.inherit_chunk_runs.size());
    buffer.inherit_chunk_ready = buffer.inherit_chunk_runs.size() < 65536;
  }
  // renut-ultimate (audit, 2026-11-07, ultimate_renut_replay_inherit_same): op 0 keeps every chunk (CopyState: the fetch
  // constants' bytes, then the tail's); a run lies in one of the two.
  static_assert(kInheritTailBegin >= kTailBegin, "an inherited tail run lies in the recorded tail");
  buffer.inherit_recorded.clear();
  if (banjo::renut_gpu::ReplayInheritSameOn() && !buffer.ops.empty() && buffer.ops[0].plain_dirty == kAllChunks &&
      !buffer.inherit_runs.empty()) {
    const u8 *image = buffer.arena.data() + buffer.ops[0].state;
    for (const auto &[offset, bytes] : buffer.inherit_runs) {
      const u32 at = offset < kVertexShaderF ? offset - kFetchBegin : kFetchBytes + (offset - kTailBegin);
      buffer.inherit_recorded.insert(buffer.inherit_recorded.end(), image + at, image + at + bytes);
    }
  }
  buffer.inherit_built = true;
}

// renut-ultimate (audit, 2026-11-07, ultimate_renut_replay_inherit_same): every inherited word of the running device as the
// recording had it (BuildInheritRuns).
uint64_t g_inherit_same_replays = 0, g_inherit_replays = 0;
bool InheritedAsRecorded(const HostCommandBuffer &buffer, const u8 *running) {
  if (buffer.inherit_recorded.empty())
    return false;
  const u8 *recorded = buffer.inherit_recorded.data();
  for (const auto &[offset, bytes] : buffer.inherit_runs) {
    if (std::memcmp(running + offset, recorded, bytes) != 0)
      return false;
    recorded += bytes;
  }
  return true;
}

void ApplyState(u8 *state, const u8 *from, uint64_t plain_dirty, uint64_t vs_dirty,
                uint64_t ps_dirty, const u8 **groups = nullptr) {
  for (uint64_t chunks = plain_dirty; chunks; chunks &= chunks - 1) {
    const u32 i = u32(std::countr_zero(chunks));
    const u32 size = ChunkSize(i);
    // Whole chunks as a fixed-size copy, which stays inline.
    if (size == kChunkBytes)
      std::memcpy(state + ChunkOffset(i), from, kChunkBytes);
    else
      std::memcpy(state + ChunkOffset(i), from, kLastChunkBytes);  // banjo-nx (2026-10-23, batch 6)
    from += size;
  }
  if (groups) {
    const u8 *const groups_from = from;
    for (const auto [dirty, slot0] : {std::pair{vs_dirty, 0u}, std::pair{ps_dirty, 64u}}) {
      ForEachGroupRun(dirty, [&](u32 first, u32 count) {
        for (u32 k = 0; k < count; ++k)
          groups[slot0 + first + k] = from + k * kConstantGroupBytes;
        from += count * kConstantGroupBytes;
      });
    }
    if (!g_group_pointers_verify)
      return;
    from = groups_from;  // (verify: the image as well, below)
  }
  for (const auto [dirty, base] : {std::pair{vs_dirty, kVertexShaderF}, std::pair{ps_dirty, kPixelShaderF}}) {
    ForEachGroupRun(dirty, [&](u32 first, u32 count) {
      // banjo-nx (2026-10-23, render-thread round): group by group, a fixed-size copy the compiler keeps inline (NEON
      // pairs), instead of a C library call for each run (most runs are one to four groups).
      u8 *to = state + base + first * kConstantGroupBytes;
      for (u32 k = 0; k < count; ++k)
        std::memcpy(to + k * kConstantGroupBytes, from + k * kConstantGroupBytes, kConstantGroupBytes);
      from += count * kConstantGroupBytes;
    });
  }
}
// banjo-nx (2026-10-28, banjo_renut_replay_group_pointers): the replay's set groups written into its image (before a
// nested run, which reads the image as its running device).
void MaterializeGroups(u8 *state, const u8 *const *groups, uint64_t vs_set, uint64_t ps_set) {
  for (u32 stage = 0; stage < 2; ++stage) {
    const u32 slot0 = stage ? 64u : 0u, base = stage ? kPixelShaderF : kVertexShaderF;
    ForEachGroupRun(stage ? ps_set : vs_set, [&](u32 first, u32 count) {
      for (u32 k = 0; k < count; ++k) {
        if (const u8 *g = groups[slot0 + first + k])
          std::memcpy(state + base + (first + k) * kConstantGroupBytes, g, kConstantGroupBytes);
      }
    });
  }
}

// banjo-nx (2026-10-27, banjo_renut_shadow_sort): one slot of a sorted entry (SortPair) into the device, as ApplyState
// copies a chunk or a constant group.
inline void ApplySortPair(u8 *state, const u8 *arena, const SortPair &p, const u8 **groups = nullptr) {
  const u8 *from = arena + p.offset;
  if (p.slot < 64) {
    u8 *to = state + ChunkOffset(p.slot);
    if (p.slot == kPlainChunks - 1)
      std::memcpy(to, from, kLastChunkBytes);
    else
      std::memcpy(to, from, kChunkBytes);
    return;
  }
  u8 *to = state + (p.slot < 128 ? kVertexShaderF : kPixelShaderF) + (p.slot & 63) * kConstantGroupBytes;
  std::memcpy(to, from, kConstantGroupBytes);
  if (groups)  // banjo-nx (2026-10-28): the group's bytes are the image's now
    groups[(p.slot < 128 ? 0u : 64u) + (p.slot & 63)] = to;
}
const u8 *g_replay_groups[kMaxRunDepth][128];  // banjo-nx (2026-10-28, banjo_renut_replay_group_pointers)
}  // namespace
bool g_group_pointers_verify = false;
uint64_t g_group_pointer_reads = 0, g_group_pointer_differ = 0;
namespace {
inline bool GroupPointersOn() {
  static const bool on = REXCVAR_GET(banjo_renut_replay_group_pointers);
  return on && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 52;
}

// banjo-nx (2026-10-27, banjo_renut_shadow_sort): whether the shaders of a sorted run's draws allow any order (each
// distinct key once: the run is sorted by it).
bool SortedRunShadersOk(const HostCommandBuffer &b, const SortRun &run) {
  uint64_t last = 0;
  for (u32 k = 0; k + 1 < run.entry_count; ++k) {  // (the last entry draws nothing)
    const Op &o = b.ops[b.sort_entries[run.first_entry + k].op];
    if (o.sort_key == last)
      continue;  // the same shaders' contents as the draw before (the record-time table answered for each object)
    last = o.sort_key;
    if (!SortableShadersLocked(o.sort_vs, o.sort_ps))
      return false;
  }
  return true;
}

void FlushDevicePending(u32 device_va);

// banjo-nx (2026-10-27, the user's "export checks first": record-time translation, banjo_renut_record_extents). The
// guest extent of every texture slot whose fetch words the op's chunks hold (TextureGuestExtent's values: the base
// level's first page and bytes, the mip chain's), worked out here on the recording thread, so a replayed draw's export
// check reads it with the op (Replay's per-depth table) instead of looking an answer up by fetch words on the render
// thread. Each recording device's thread has a memo of its own by the fetch words (textures repeat); a slot
// covers fetch bytes [24 r, 24 r + 24), and the first op of a buffer (every chunk) gives all 26.
struct ExtentMemoEntry {
  u32 words[6];
  SlotExtent e;
  bool valid = false;
};
struct ExtentMemo {
  // banjo-nx (2026-10-28, cache-miss audit 2): each slot's last words and extent, looked at first: a chunk change marks
  // two or three slots whose words did not change, and the memo's entry (12 KB a device) was a miss at most of them
  // (0.65 % of the recording threads in the c4 console run).
  ExtentMemoEntry last[d3d::kTextureFetchSlots];
  // blur-nx (2026-11-13, M-32 of docs/cpu-moonshots-50-1113.md): two ways a set (the latest first), where one entry a
  // set made two textures that share the hash's top byte evict each other: each miss runs GetTextureDataSizes and
  // GetTextureLevelLayout per mip (0.2 ms a frame of the recording threads' samples in the console profiles).
  ExtentMemoEntry entry[256][2];
};
ExtentMemo g_extent_memos[native::kTouchSlots];

// False (nothing written) for words that are not a texture's (the last slots share their bytes with vertex fetch
// constants): such a slot is left out of the record, and a draw that samples it takes the answers table.
inline bool SlotExtentOf(const d3d::TextureFetchConstant &guest, ExtentMemo *memo, u32 slot, SlotExtent &out) {
  const TextureFetch fetch = TextureFetch::FromGuest(guest);
  if (!fetch.IsTexture())
    return false;
  ExtentMemoEntry *m = nullptr;
  ExtentMemoEntry *last = memo ? &memo->last[slot] : nullptr;
  if (last && last->valid && SameWords<6>(last->words, fetch.dword)) {
    out = last->e;
    return true;
  }
  if (memo) {
    uint64_t h = 0;
    for (u32 i = 0; i < 6; ++i)
      h = (h ^ fetch.dword[i]) * 0x9E3779B97F4A7C15ull;
    ExtentMemoEntry *set = memo->entry[u32(h >> 56)];
    if (set[0].valid && SameWords<6>(set[0].words, fetch.dword)) {
      out = set[0].e;
      *last = set[0];
      return true;
    }
    if (set[1].valid && SameWords<6>(set[1].words, fetch.dword)) {  // blur-nx (2026-11-13, M-32)
      std::swap(set[0], set[1]);
      out = set[0].e;
      *last = set[0];
      return true;
    }
    set[1] = set[0];  // (the older way leaves; the new entry goes first, below)
    m = &set[0];
  }
  // TextureGuestExtent's computation (render/textures.cpp), without its render-thread memo.
  u32 base_bytes = 0, mip_bytes = 0;
  GetTextureDataSizes(fetch, base_bytes, mip_bytes);
  out.pages[0] = fetch.BasePage();
  out.sizes[0] = base_bytes;
  out.pages[1] = mip_bytes ? fetch.MipPage() : 0;
  out.sizes[1] = mip_bytes;
  if (m) {
    std::memcpy(m->words, fetch.dword, sizeof(m->words));
    m->e = out;
    m->valid = true;
    *last = *m;
  }
  return true;
}

// The texture slots the fetch chunks in 'chunks' (bit c = fetch bytes [64 c, 64 c + 64)) hold any word of.
// banjo-nx (2026-10-27, ARM pass): a chunk's slots from a table, one lookup a changed chunk.
constexpr std::array<u32, kFetchChunks> kSlotsOfChunk = [] {
  std::array<u32, kFetchChunks> t{};
  for (u32 r = 0; r < d3d::kTextureFetchSlots; ++r) {
    t[r * 24 / kChunkBytes] |= 1u << r;
    t[(r * 24 + 23) / kChunkBytes] |= 1u << r;
  }
  return t;
}();
inline u32 SlotsOfFetchChunks(uint64_t chunks) {
  u32 slots = 0;
  for (uint64_t bits = chunks & ((uint64_t(1) << kFetchChunks) - 1); bits; bits &= bits - 1)
    slots |= kSlotsOfChunk[std::countr_zero(bits)];
  return slots;
}
static_assert(sizeof(d3d::TextureFetchConstant) == 24 && d3d::kTextureFetchSlots * 24 <= kFetchBytes);

// The op's extents record in the arena: the slots its chunks changed, those of them that hold a texture's words, then
// each of those slots' SlotExtent, in slot order.
u32 RecordExtents(HostCommandBuffer &buffer, const u8 *device, uint64_t plain_dirty, u32 touch_slot) {
  const u32 changed = SlotsOfFetchChunks(plain_dirty & ((uint64_t(1) << kFetchChunks) - 1));
  if (!changed)
    return kNoExtents;
  ExtentMemo *memo = touch_slot - 1 < native::kTouchSlots - 1 ? &g_extent_memos[touch_slot] : nullptr;
  const auto &fetch = reinterpret_cast<const d3d::Device *>(device)->m_Constants.TextureFetch;
  SlotExtent found[d3d::kTextureFetchSlots];
  u32 slots = 0, count = 0;
  for (u32 bits = changed; bits; bits &= bits - 1) {
    const u32 r = u32(std::countr_zero(bits));
    if (SlotExtentOf(fetch[r], memo, r, found[count])) {
      slots |= 1u << r;
      ++count;
    }
  }
  // The record says which slots it changed (all of 'changed': those it leaves out are unknown from here on).
  auto &arena = buffer.arena;
  const size_t offset = (arena.size() + 3) & ~size_t(3);
  GrowArena(arena, offset + 8 + size_t(count) * sizeof(SlotExtent));
  u8 *to = arena.data() + offset;
  std::memcpy(to, &changed, 4);
  std::memcpy(to + 4, &slots, 4);
  if (count)
    std::memcpy(to + 8, found, size_t(count) * sizeof(SlotExtent));
  return u32(offset);
}

// banjo-nx (2026-10-27, the user's Phase A: resources resolved when bound). The op's resource record: the texture
// objects of the slots whose address word or fetch words its chunks changed (FindTextureObject: the guest header's
// words read here, on the recording thread, where the game has just written them), and for a draw its index buffer
// header's words (what the hardware's command stream takes at the draw). Replay keeps them per depth (DrawResources)
// and the replayed draw reads them instead of guest headers.
constexpr u32 kNoResources = 0xFFFFFFFFu;
constexpr u32 kTextureVaChunk0 = kFetchChunks + (d3d::kDeviceOffsetTextures - kTailBegin) / kChunkBytes;
static_assert(d3d::kDeviceOffsetTextures >= kTailBegin && (d3d::kDeviceOffsetTextures - kTailBegin) % 4 == 0);
// The texture slots a plain chunk holds any word of (their address word or their fetch words).
constexpr std::array<u32, kPlainChunks> kTextureSlotsOfChunk = [] {
  std::array<u32, kPlainChunks> t{};
  for (u32 r = 0; r < d3d::kTextureFetchSlots; ++r) {
    t[r * 24 / kChunkBytes] |= 1u << r;
    t[(r * 24 + 23) / kChunkBytes] |= 1u << r;
    const u32 va_at = d3d::kDeviceOffsetTextures + r * 4 - kTailBegin;
    t[kFetchChunks + va_at / kChunkBytes] |= 1u << r;
  }
  return t;
}();
struct ResourceHead {
  u32 slots = 0;   // the slots the record gives (bit r), in slot order after the head (8 bytes each)
  u32 flags = 0;   // bit 0: ib holds the draw's index buffer header words
  u32 ib[3] = {};
  u32 vb_mask = 0;  // a draw's stream sources whose header words follow the objects (bit s, 8 bytes each)
};
static_assert(sizeof(ResourceHead) == 24);
// banjo-nx (2026-10-28, ARM audit): bit i set for each of the 16 words at 'p' that is not zero (any byte order): one NEON
// compare of four vectors and a narrowing, where a loop tested the words one at a time.
inline u32 SetWordsOf16(const u8 *p) {
#if defined(__aarch64__)
  const uint32x4_t a = vreinterpretq_u32_u8(vld1q_u8(p)), b = vreinterpretq_u32_u8(vld1q_u8(p + 16));
  const uint32x4_t c = vreinterpretq_u32_u8(vld1q_u8(p + 32)), d = vreinterpretq_u32_u8(vld1q_u8(p + 48));
  // Each word's zero test as a byte (0xFF zero), in word order, then those bytes' top bits as a mask.
  const uint16x8_t ab = vcombine_u16(vmovn_u32(vceqzq_u32(a)), vmovn_u32(vceqzq_u32(b)));
  const uint16x8_t cd = vcombine_u16(vmovn_u32(vceqzq_u32(c)), vmovn_u32(vceqzq_u32(d)));
  const uint8x16_t zero = vcombine_u8(vmovn_u16(ab), vmovn_u16(cd));
  static constexpr u8 kBits[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
  const uint8x16_t bits = vandq_u8(zero, vld1q_u8(kBits));
  const u32 zeros = u32(vaddv_u8(vget_low_u8(bits))) | u32(vaddv_u8(vget_high_u8(bits))) << 8;
  return ~zeros & 0xFFFFu;
#else
  u32 set = 0;
  for (u32 i = 0; i < 16; ++i) {
    u32 w;
    std::memcpy(&w, p + i * 4, 4);
    set |= u32(w != 0) << i;
  }
  return set;
#endif
}

// banjo-nx (2026-10-28, banjo_renut_record_lookups): each recording device's last objects (its thread's alone, as the
// extent memos): a texture slot's by its address and header words, a stream's by its key, the index buffer's by its
// header words. Objects are never freed, so a remembered one stays good.
struct ResourceMemo {
  struct Tex {
    u32 va = 0;
    u32 header[6] = {};
    TextureObject *obj = nullptr;
  };
  struct Geo {
    u32 key[6] = {};
    GeometryObject *obj = nullptr;
  };
  Tex tex[d3d::kTextureFetchSlots];
  Geo stream[16];
  u32 ib[3] = {};
  GeometryObject *ib_obj = nullptr;
  // banjo-nx (2026-10-28, ARM audit): the 10 s line's counts, the device's thread's alone (a load and a store; shared
  // counters took an exclusive-access loop and moved their line between the two recording threads' cores at every op).
  std::atomic<uint64_t> hits{0}, tables{0};
};
ResourceMemo g_resource_memos[native::kTouchSlots];
inline void BumpOwn(std::atomic<uint64_t> &c, uint64_t n) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

// banjo-nx (2026-10-28, per-draw night): RecordResources in two calls around the op's state copy (NewOp): the plan
// (the guest headers' lines, then the keys, the memo's objects and the tables' lines asked for) before CopyState and
// RecordExtents, the table lookups and the record after them, so that the table lines' misses overlap the copy (the
// lookups followed the prefetches at once: the d4 console profile still had them at 1.4 % of the recording threads).
struct ResourcePlan {
  ResourceHead head;
  u32 slots = 0;
  ResourceMemo *memo = nullptr;
  u32 object_mask = 0, stream_lookups = 0, tex_lookups = 0;
  GeometryObject *index_object = nullptr;
  uint64_t index_hash = 0, memo_hits = 0;
  bool index_lookup = false;
  // Written where the masks say (no initialisers: 1.5 KB an op).
  u32 tex_va[d3d::kTextureFetchSlots];
  u32 vb_words[16][2];
  GeometryObject *stream_objects[16];
  u32 stream_keys[16][6];
  uint64_t stream_hash[16];
  TextureObject *tex_objects[d3d::kTextureFetchSlots];
  u32 tex_words[d3d::kTextureFetchSlots][6];
  uint64_t tex_hash[d3d::kTextureFetchSlots];
};
void PlanResources(ResourcePlan &p, const u8 *device, uint64_t plain_dirty, OpKind kind, u32 touch_slot) {
  u32 &slots = p.slots;
  ResourceHead &head = p.head;
  u32 (&tex_va)[d3d::kTextureFetchSlots] = p.tex_va;
  u32 (&vb_words)[16][2] = p.vb_words;
  GeometryObject *(&stream_objects)[16] = p.stream_objects;
  u32 &object_mask = p.object_mask;
  GeometryObject *&index_object = p.index_object;
  u32 (&stream_keys)[16][6] = p.stream_keys;
  uint64_t (&stream_hash)[16] = p.stream_hash;
  u32 &stream_lookups = p.stream_lookups;
  uint64_t &index_hash = p.index_hash;
  bool &index_lookup = p.index_lookup;
  uint64_t &memo_hits = p.memo_hits;
  TextureObject *(&tex_objects)[d3d::kTextureFetchSlots] = p.tex_objects;
  u32 (&tex_words)[d3d::kTextureFetchSlots][6] = p.tex_words;
  uint64_t (&tex_hash)[d3d::kTextureFetchSlots] = p.tex_hash;
  u32 &tex_lookups = p.tex_lookups;
  for (uint64_t bits = plain_dirty & kAllChunks; bits; bits &= bits - 1)
    slots |= kTextureSlotsOfChunk[std::countr_zero(bits)];
  head.slots = slots;
  const bool draw = kind == OpKind::Draw;
  // banjo-nx (2026-10-28, banjo_renut_record_lookups / A/B 50): the lookups in three steps, each step's misses together
  // (the guest headers' lines, then the tables' lines), and the device's memo first. Off: one lookup after the other,
  // as before.
  const bool batch = LookupTablesOn() && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 50;
  ResourceMemo *const memo = p.memo =
      batch && touch_slot - 1 < native::kTouchSlots - 1 ? &g_resource_memos[touch_slot] : nullptr;
  const auto word = [device](u32 offset) {
    u32 v;
    std::memcpy(&v, device + offset, 4);
    return __builtin_bswap32(v);
  };
  // Step 1: the guest headers' addresses (the slots', the index buffer's, the bound stream sources').
  const d3d::BaseTexture *tex_header[d3d::kTextureFetchSlots];
  for (u32 bits = slots; bits; bits &= bits - 1) {
    const u32 r = u32(std::countr_zero(bits));
    tex_va[r] = word(d3d::kDeviceOffsetTextures + r * 4);
    tex_header[r] = tex_va[r] ? GuestPtr<const d3d::BaseTexture>(tex_va[r]) : nullptr;
    if (batch && tex_header[r])
      __builtin_prefetch(&tex_header[r]->Format);
  }
  const d3d::IndexBuffer *ib = nullptr;
  const d3d::VertexBuffer *vb[16];
  u32 vb_set = 0;  // the streams with a source (vb[s] set)
  if (draw) {
    ib = GuestPtr<const d3d::IndexBuffer>(word(d3d::kDeviceOffsetIndexBuffer));
    if (batch && ib)
      __builtin_prefetch(ib);
    // banjo-nx (2026-10-28, ARM audit): the 16 source words' non-zero ones found at once (most draws bind one or two).
    const u32 sources = SetWordsOf16(device + d3d::kDeviceOffsetStreamSources);
    for (u32 bits = sources; bits; bits &= bits - 1) {
      const u32 s = u32(std::countr_zero(bits));
      u32 sva;
      std::memcpy(&sva, device + d3d::kDeviceOffsetStreamSources + s * 4, 4);
      vb[s] = GuestPtr<const d3d::VertexBuffer>(__builtin_bswap32(sva));
      if (vb[s]) {
        vb_set |= 1u << s;
        if (batch)
          __builtin_prefetch(&vb[s]->Format);
      }
    }
  }
  // Step 2: the headers' words and the keys; the memo's objects; for the others the hash (which fetches the table line).
  if (draw) {
    if (ib) {
      head.ib[0] = u32(ib->resource.Common);
      head.ib[1] = u32(ib->Address);
      head.ib[2] = u32(ib->Size);
      head.flags |= 1;
    }
    // The bound stream sources' vertex buffer headers (their CPU base and size: GetStreamSource reads them).
    for (u32 bits = vb_set; bits; bits &= bits - 1) {
      const u32 s = u32(std::countr_zero(bits));
      vb_words[s][0] = u32(vb[s]->Format.dword[0]);
      vb_words[s][1] = u32(vb[s]->Format.dword[1]);
    }
    head.vb_mask = vb_set;
  }
  // banjo-nx (2026-10-27, geometry objects): a draw's stream sources and index buffer as objects (render/draw.h
  // FindStreamObject: the stream's fetch words, source, stride byte | stream << 8, the source header's two words).
  // flags bit 1: a word of their mask (and a pad word) and their pointers follow the header words; bit 2: then the
  // index buffer's.
  if (draw && GeometryObjectsRecorded()) {
    for (u32 bits = head.vb_mask; bits; bits &= bits - 1) {
      const u32 s = u32(std::countr_zero(bits));
      const u32 fetch_index = d3d::kFirstVertexFetchIndex + d3d::StreamFetchIndex(s);
      const u32 constants = u32(offsetof(d3d::Device, m_Constants)) + fetch_index * 8;
      u32 *key = stream_keys[s];
      key[0] = word(constants);
      key[1] = word(constants + 4);
      key[2] = word(d3d::kDeviceOffsetStreamSources + s * 4);
      key[3] = u32(device[d3d::kDeviceOffsetStreamStrides + s]) | s << 8;
      key[4] = vb_words[s][0];
      key[5] = vb_words[s][1];
      if (memo && memo->stream[s].obj && SameWords<6>(memo->stream[s].key, key)) {
        stream_objects[s] = memo->stream[s].obj;
        object_mask |= 1u << s;
        ++memo_hits;
      } else if (batch) {
        stream_hash[s] = StreamObjectHash(key);
        stream_lookups |= 1u << s;
      } else if (GeometryObject *o = FindStreamObject(key)) {
        stream_objects[s] = o;
        object_mask |= 1u << s;
      }
    }
    if (head.flags & 1) {
      if (memo && memo->ib_obj && SameWords<3>(memo->ib, head.ib)) {
        index_object = memo->ib_obj;
        ++memo_hits;
      } else if (batch) {
        index_hash = IndexObjectHash(head.ib);
        index_lookup = true;
      } else {
        index_object = FindIndexObject(head.ib);
      }
    }
  }
  for (u32 bits = slots; bits; bits &= bits - 1) {
    const u32 r = u32(std::countr_zero(bits));
    tex_objects[r] = nullptr;
    if (!tex_header[r])
      continue;
    std::memcpy(tex_words[r], &tex_header[r]->Format, sizeof(tex_words[r]));
    if (memo && memo->tex[r].obj && memo->tex[r].va == tex_va[r] && SameWords<6>(memo->tex[r].header, tex_words[r])) {
      tex_objects[r] = memo->tex[r].obj;
      ++memo_hits;
    } else if (batch) {
      tex_hash[r] = TextureObjectHash(tex_va[r], tex_words[r]);
      tex_lookups |= 1u << r;
    } else {
      tex_objects[r] = FindTextureObject(tex_va[r], tex_words[r]);
    }
  }
}
u32 FinishResources(HostCommandBuffer &buffer, ResourcePlan &p) {
  const u32 slots = p.slots;
  ResourceHead &head = p.head;
  ResourceMemo *const memo = p.memo;
  u32 &object_mask = p.object_mask;
  GeometryObject *&index_object = p.index_object;
  const u32 stream_lookups = p.stream_lookups, tex_lookups = p.tex_lookups;
  const bool index_lookup = p.index_lookup;
  // Step 3: the tables (their lines asked for in step 2); the memo takes what they gave.
  for (u32 bits = stream_lookups; bits; bits &= bits - 1) {
    const u32 s = u32(std::countr_zero(bits));
    if (GeometryObject *o = FindStreamObjectAt(p.stream_hash[s], p.stream_keys[s])) {
      p.stream_objects[s] = o;
      object_mask |= 1u << s;
      if (memo) {
        std::memcpy(memo->stream[s].key, p.stream_keys[s], 24);
        memo->stream[s].obj = o;
      }
    }
  }
  if (index_lookup) {
    index_object = FindIndexObjectAt(p.index_hash, head.ib);
    if (memo && index_object) {
      std::memcpy(memo->ib, head.ib, 12);
      memo->ib_obj = index_object;
    }
  }
  for (u32 bits = tex_lookups; bits; bits &= bits - 1) {
    const u32 r = u32(std::countr_zero(bits));
    p.tex_objects[r] = FindTextureObjectAt(p.tex_hash[r], p.tex_va[r], p.tex_words[r]);
    if (memo && p.tex_objects[r]) {
      memo->tex[r].va = p.tex_va[r];
      std::memcpy(memo->tex[r].header, p.tex_words[r], sizeof(p.tex_words[r]));
      memo->tex[r].obj = p.tex_objects[r];
    }
  }
  if (memo) {
    if (p.memo_hits)
      BumpOwn(memo->hits, p.memo_hits);
    if (const u32 tables = u32(std::popcount(stream_lookups) + std::popcount(tex_lookups)) + (index_lookup ? 1u : 0u))
      BumpOwn(memo->tables, tables);
  }
  if (object_mask)
    head.flags |= 2;
  if (index_object)
    head.flags |= 4;
  if (!slots && !head.flags && !head.vb_mask)
    return kNoResources;
  auto &arena = buffer.arena;
  const size_t offset = (arena.size() + 7) & ~size_t(7);
  GrowArena(arena, offset + sizeof(head) + size_t(std::popcount(slots)) * sizeof(TextureObject *) +
               size_t(std::popcount(head.vb_mask)) * 8 + ((head.flags & 2) ? 8 : 0) +
               size_t(std::popcount(object_mask)) * sizeof(GeometryObject *) +
               ((head.flags & 4) ? sizeof(GeometryObject *) : 0));
  u8 *to = arena.data() + offset;
  std::memcpy(to, &head, sizeof(head));
  to += sizeof(head);
  for (u32 bits = slots; bits; bits &= bits - 1) {
    std::memcpy(to, &p.tex_objects[std::countr_zero(bits)], sizeof(TextureObject *));
    to += sizeof(TextureObject *);
  }
  for (u32 bits = head.vb_mask; bits; bits &= bits - 1) {
    std::memcpy(to, p.vb_words[std::countr_zero(bits)], 8);
    to += 8;
  }
  if (head.flags & 2) {
    const u32 mask_words[2] = {object_mask, 0};
    std::memcpy(to, mask_words, 8);
    to += 8;
    for (u32 bits = object_mask; bits; bits &= bits - 1) {
      std::memcpy(to, &p.stream_objects[std::countr_zero(bits)], sizeof(GeometryObject *));
      to += sizeof(GeometryObject *);
    }
  }
  if (head.flags & 4)
    std::memcpy(to, &index_object, sizeof(index_object));
  return u32(offset);
}
u32 RecordResources(HostCommandBuffer &buffer, const u8 *device, uint64_t plain_dirty, OpKind kind, u32 touch_slot) {
  ResourcePlan p;
  PlanResources(p, device, plain_dirty, kind, touch_slot);
  return FinishResources(buffer, p);
}
// banjo-nx (2026-10-28, ARM audit): Replay's stream, for the next op: its objects' lines (its record came an op before).
// Trimmed after the d4 console profile (the helper 1.2 ms a frame, many prefetches an op): an object the replay holds
// already for that slot or stream (in use an op ago: in the cache) is skipped.
void PrefetchResourceObjects(const u8 *record, const DrawResources &held) {
  ResourceHead head;
  std::memcpy(&head, record, sizeof(head));
  const u8 *from = record + sizeof(head);
  for (u32 bits = head.slots; bits; bits &= bits - 1, from += sizeof(TextureObject *)) {
    const u32 r = u32(std::countr_zero(bits));
    TextureObject *obj;
    std::memcpy(&obj, from, sizeof(obj));
    if (obj && !(((held.mask >> r) & 1) && held.slot[r] == obj))
      PrefetchTextureObject(obj);
  }
  from += size_t(std::popcount(head.vb_mask)) * 8;
  if (head.flags & 2) {
    u32 object_mask;
    std::memcpy(&object_mask, from, 4);
    from += 8;
    for (u32 bits = object_mask; bits; bits &= bits - 1, from += sizeof(GeometryObject *)) {
      const u32 s = u32(std::countr_zero(bits));
      GeometryObject *o;
      std::memcpy(&o, from, sizeof(o));
      if (!(((held.obj_mask >> s) & 1) && held.streams[s] == o))
        PrefetchGeometryObject(o, false);
    }
  }
  if (head.flags & 4) {
    GeometryObject *o;
    std::memcpy(&o, from, sizeof(o));
    if (o != held.index_obj)
      PrefetchGeometryObject(o, true);
  }
}
// 'copies' (the stream on): the objects' lines came an op before (PrefetchResourceObjects): the copies their answers
// name are fetched; else the answers' lines, as before.
inline void ApplyResources(DrawResources &table, const u8 *record, bool draw, bool copies) {
  ResourceHead head;
  std::memcpy(&head, record, sizeof(head));
  const u8 *from = record + sizeof(head);
  for (u32 bits = head.slots; bits; bits &= bits - 1) {
    const u32 r = u32(std::countr_zero(bits));
    TextureObject *obj;
    std::memcpy(&obj, from, sizeof(obj));
    from += sizeof(obj);
    table.slot[r] = obj;
    table.mask = obj ? table.mask | (1u << r) : table.mask & ~(1u << r);
  }
  // A draw's own index buffer and vertex buffer words (a draw without them has none: never an earlier draw's).
  if (draw) {
    table.ib_valid = (head.flags & 1) != 0;
    table.vb_mask = head.vb_mask;
  }
  if (head.flags & 1)
    std::memcpy(table.ib, head.ib, sizeof(table.ib));
  for (u32 bits = head.vb_mask; bits; bits &= bits - 1) {
    std::memcpy(table.vb[std::countr_zero(bits)], from, 8);
    from += 8;
  }
  // banjo-nx (2026-10-27, geometry objects): a draw's own (none from an earlier draw).
  u32 object_mask = 0;
  if (head.flags & 2) {
    std::memcpy(&object_mask, from, 4);
    from += 8;
    for (u32 bits = object_mask; bits; bits &= bits - 1) {
      GeometryObject *&o = table.streams[std::countr_zero(bits)];
      std::memcpy(&o, from, sizeof(GeometryObject *));
      from += sizeof(GeometryObject *);
      if (copies && draw) {
        PrefetchGeometryCopies(o, false);
      } else {
        __builtin_prefetch(reinterpret_cast<const char *>(o) + 64);  // its answers, read once the draw reaches geometry
        __builtin_prefetch(reinterpret_cast<const char *>(o) + 128);
      }
    }
  }
  if (draw) {
    table.obj_mask = object_mask;
    table.index_obj = nullptr;
    if (head.flags & 4) {
      std::memcpy(&table.index_obj, from, sizeof(table.index_obj));
      if (copies)
        PrefetchGeometryCopies(table.index_obj, true);
      else
        __builtin_prefetch(reinterpret_cast<const char *>(table.index_obj) + 64);
    }
  }
}
DrawResources g_replay_resources[kMaxRunDepth];

// Replay's side: the record into the depth's table.
inline void ApplyExtents(DrawSlotExtents &table, const u8 *record) {
  u32 changed, slots;
  std::memcpy(&changed, record, 4);
  std::memcpy(&slots, record + 4, 4);
  const u8 *from = record + 8;
  for (u32 bits = slots; bits; bits &= bits - 1) {
    std::memcpy(&table.slot[std::countr_zero(bits)], from, sizeof(SlotExtent));
    from += sizeof(SlotExtent);
  }
  table.mask = (table.mask & ~changed) | slots;
}
DrawSlotExtents g_replay_extents[kMaxRunDepth];

// A new op on the device's buffer, with the device's state and the constant
// groups set so far, after the device's pending Begin*Vertices data. Consumes
// the device's dirty masks the way the original calls do when they write the
// state into the buffer. g_record_mutex is held for the lookups only: N&B
// records on several threads at once, and the snapshot copy, most of the
// work, goes into the device's own buffer, which only its recording thread
// touches until the buffer runs.
// banjo-nx (2026-10-28, host packets): the state keys are made while packets or their census are on.
inline bool HostPacketKeysOn() {
  return REXCVAR_GET(banjo_renut_host_packets) || REXCVAR_GET(banjo_renut_diag_packet_census);
}
// banjo-nx (2026-10-28, host packets): a chunk's part of the state key: its content's 64-bit hash (the PMULL CRC-64 the
// stream and texture checks use) with its index, through a 64-bit finaliser (a bijection, so the XOR of the chunks' parts
// is not linear in their bytes the way the CRCs alone would be).
inline uint64_t ChunkKeyPartAt(const u8 *chunk, u32 size, u32 i) {
  uint64_t h = banjo::renut_gpu::ContentHash64(chunk, size) ^ (uint64_t(i + 1) * 0x9E3779B97F4A7C15ull);
  h ^= h >> 33;
  h *= 0xFF51AFD7ED558CCDull;
  h ^= h >> 33;
  h *= 0xC4CEB9FE1A85EC53ull;
  h ^= h >> 33;
  return h;
}
inline uint64_t ChunkKeyPart(const u8 *bytes, u32 i) { return ChunkKeyPartAt(bytes + ChunkOffset(i), ChunkSize(i), i); }

// banjo-nx (2026-10-28, host packets, second form): the key covers only the device words a packet's answers read. The
// first form keyed the whole state and found the same state again for 5.6 % of the draws (the title screen): words
// the answers never read (the streams, index buffer and vertex fetch constants, the texture fetch words, private
// words) changed from frame to frame. The fields are the lean executor's own dependency sets for those answers (and
// banjo_renut_fast_draw_verify checks them): the shaders and declaration, the targets, the window/values/control
// packets, the destination packet, the bools, the word at 0x29C0 and the predication. Word by word (a mixed chunk's
// other words left out), from the field tables.
constexpr u32 kPacketFields = kFieldShaders | kFieldDecl | kFieldTargets | kFieldViewPackets | kFieldBools |
                              kFieldDest | kField29C0 | kFieldPredication;
struct PacketWordTables {
  uint16_t words[64] = {};  // bit w: word w of the chunk is read
  uint64_t chunks = 0;      // the chunks with any
};
constexpr PacketWordTables MakePacketWordTables() {
  PacketWordTables t{};
  for (u32 c = 0; c < kPlainChunks; ++c) {
    for (u32 w = 0; w < ChunkSize(c) / 4; ++w)
      if (kPlainFields.word[c][w] & kPacketFields)
        t.words[c] |= uint16_t(1u << w);
    if (t.words[c])
      t.chunks |= uint64_t(1) << c;
  }
  return t;
}
constexpr PacketWordTables kPacketWords = MakePacketWordTables();
// A chunk's part of the packet key: its packet words (the others as zeros).
inline uint64_t PacketChunkPart(const u8 *bytes, u32 i) {
  const u32 size = ChunkSize(i), mask = kPacketWords.words[i];
  const u8 *src = bytes + ChunkOffset(i);
  if (mask == (1u << (size / 4)) - 1u)
    return ChunkKeyPartAt(src, size, i);
  alignas(16) u8 kept[kChunkBytes] = {};
  for (u32 m = mask; m; m &= m - 1) {
    const u32 w = u32(std::countr_zero(m));
    std::memcpy(kept + 4 * w, src + 4 * w, 4);
  }
  return ChunkKeyPartAt(kept, size, i);
}
// The key a draw's packet is found by: the op's state key with what of the call the derived state reads (the
// primitive, indexed or not, tessellation). Never 0 (0: no key).
inline uint64_t PacketKeyOf(const OpCall &c) {
  if (!c.state_key)
    return 0;
  const uint64_t k = c.state_key ^ ((uint64_t(c.primitive) | uint64_t(c.indexed) << 8 | uint64_t(c.tessellation) << 9) *
                                    0xD6E8FEB86659FD93ull);
  return k ? k : 1;
}

// blur-nx (2026-11-10, port 76; its key 215: blur_renut_stack_surface_headers): the headers recorded with op 'op_index',
// or nullptr.
const StackHeaderSet *StackHeadersOf(const HostCommandBuffer &buffer, size_t op_index) {
  const auto &list = buffer.stack_headers;
  if (list.empty())
    return nullptr;
  const auto it = std::lower_bound(list.begin(), list.end(), op_index,
                                   [](const std::pair<u32, StackHeaderSet> &e, size_t i) { return e.first < i; });
  return it != list.end() && it->first == op_index ? &it->second : nullptr;
}

Op *NewOp(u32 device_va, OpKind kind, HostCommandBuffer **out_buffer) {
  ::renut::gpu::census::Scope census_scope(::renut::gpu::census::kNewOp);  // blur-nx (2026-11-17)
  const u32 command_buffer = RecordingBuffer(device_va);
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!command_buffer || !device)
    return nullptr;
  HostCommandBuffer *found = nullptr;
  DeviceRecording *found_set = nullptr;
  const u32 touch_slot = native::TouchSlotOf(device_va);
  RecordCache &cache = touch_slot - 1 < native::kTouchSlots - 1 ? g_record_cache_slots[touch_slot].c : t_record_cache;
  const bool memo = banjo::renut_gpu::CpuMemosOn();
  static const bool local = REXCVAR_GET(blur_renut_record_memo_local);
  if (memo && cache.device_va == device_va && cache.command_buffer == command_buffer &&
      (local ? cache.cb_generation == CbGeneration(command_buffer).load(std::memory_order_acquire) &&
                   cache.set->pending.load(std::memory_order_acquire) == 0
             : cache.generation == g_record_generation.load(std::memory_order_acquire) &&
                   g_pending_count.load(std::memory_order_acquire) == 0)) {
    found = cache.buffer;
    found_set = cache.set;
    ++cache.hits;  // blur-nx (2026-11-17): the census
  } else {
    if (cache.device_va != device_va || cache.command_buffer != command_buffer || !cache.set)  // blur-nx: the census
      ++cache.miss_other;
    else if (cache.cb_generation != CbGeneration(command_buffer).load(std::memory_order_relaxed))
      ++cache.miss_generation;
    else
      ++cache.miss_pending;
    std::lock_guard lock(g_record_mutex);
    FlushDevicePending(device_va);
    auto &slot = g_buffers[command_buffer];
    if (!slot) {
      slot = NewBuffer();
      BumpRecordGeneration(command_buffer);
    }
    found = slot.get();
    found_set = &g_recording_devices[device_va];
    if (memo) {
      cache.device_va = device_va;  // blur-nx (2026-11-17): field by field, the census kept
      cache.command_buffer = command_buffer;
      cache.generation = g_record_generation.load(std::memory_order_relaxed);
      cache.cb_generation = CbGeneration(command_buffer).load(std::memory_order_relaxed);
      cache.buffer = found;
      cache.set = found_set;
    }
  }
  HostCommandBuffer &buffer = *found;
  DeviceRecording &set = *found_set;
  const uint64_t vs_dirty = u64(device->m_Pending[0]);
  const uint64_t ps_dirty = u64(device->m_Pending[1]);
  // banjo-nx (2026-10-26, banjo_renut_tags): the other tags as chunks, before they are cleared (native::TagChunks).
  const uint64_t tags[3] = {u64(device->m_Pending[2]), u64(device->m_Pending[3]), u64(device->m_Pending[4])};
  const uint64_t tag_chunks = native::TagChunks(tags[0], tags[1], tags[2]);
  set.vs |= vs_dirty;
  set.ps |= ps_dirty;
  for (auto &mask : device->m_Pending)
    mask = 0;
  ForgetSnapshotDeltas(device);  // banjo-nx (2026-10-20, from sa2-nx): the masks went to the buffer, not to the next delta (queued.cpp)

  // blur-nx (2026-11-18, the Renut host batch's census, #20): an op vector that grew at this op.
  const bool hb_census = ::renut::gpu::host_batch::CensusOn();
  const size_t ops_capacity = hb_census ? buffer.ops.capacity() : 0;
  Op &op = buffer.ops.emplace_back();
  if (hb_census) {
    ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kOps);
    if (buffer.ops.capacity() != ops_capacity)
      ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kOpGrowths);
  }
  op.kind = kind;
  op.vs_set = set.vs;
  op.ps_set = set.ps;
  // The first op of a buffer follows ops of other buffers: nothing known.
  const bool first = buffer.ops.size() == 1;
  const auto *bytes = reinterpret_cast<const u8 *>(device);
  // halo3-nx (2026-11-03): NoteSetWords. blur-nx (2026-11-13): after 'changed' below (NoteSetWordsOf).
  {
    StackHeaderSet headers;  // blur-nx (2026-11-10, port 76; its key 215: blur_renut_stack_surface_headers)
    CaptureStackHeaders(bytes, headers);
    if (headers.count) {
      buffer.stack_headers.emplace_back(u32(buffer.ops.size() - 1), headers);
      if (hb_census)  // blur-nx (2026-11-18, the census's #21)
        ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kOpsStackHeaders);
    }
  }
  if (kind == OpKind::Draw) {
    u32 vs_word;
    std::memcpy(&vs_word, bytes + d3d::kDeviceOffsetVertexShader, 4);
    op.binding = LastBindingOf(__builtin_bswap32(vs_word));
  }
  // banjo-nx (2026-10-23, batch 6b): fast_chunks. banjo-nx (2026-10-26): one static (one guard test an op, not three).
  struct ChunkConfig {
    bool fast_chunks, tags;
    u32 check_period;
  };
  static const ChunkConfig config{ChangedChunksSelfTest(), REXCVAR_GET(banjo_renut_tags),
                                  u32(std::max(0, int(REXCVAR_GET(banjo_renut_tags_check))))};
  const bool fast_chunks = config.fast_chunks;
  const u32 check_period = config.check_period;
  // banjo-nx (2026-10-26, banjo_renut_tags): the chunks the setters marked since the device's previous op (taken every
  // op, so a whole compare clears them too); only those compared when the shadow is primed.
  uint64_t marked = kAllChunks;
  const bool tracked = config.tags && native::TakeTouches(device_va, &marked);
  const uint64_t setter_marked = marked;  // banjo-nx (2026-10-26): before the inline writes' tags (the census)
  marked |= tag_chunks;
  const bool from_marks = tracked && set.primed && fast_chunks && !g_tags_failed.load(std::memory_order_relaxed) &&
                          banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 21;
  set.primed = tracked;
  uint64_t changed;
  // banjo-nx (2026-10-28, per-draw night): the marked chunks' shadow lines asked for together (the device's lines the
  // game has just written; the shadow's were the compare's misses, 1.6 % of the recording threads in the d4 profile).
  if (from_marks) {
    for (uint64_t bits = marked & kAllChunks; bits; bits &= bits - 1)
      __builtin_prefetch(set.shadow + ChunkOffset(u32(std::countr_zero(bits))));
  }
  // banjo-nx (2026-10-26, Unleashed-style step 1): the mixed chunks' changed words, before the shadow takes them.
  u32 fields = MixedChunkFields(bytes, set.shadow, from_marks ? marked & kAllChunks : kAllChunks);
  if (from_marks) {
    changed = ChangedChunksMarked(bytes, set.shadow, marked & kAllChunks);
    if (check_period && ++set.unchecked >= check_period) {
      set.unchecked = 0;
      ++set.counts.checks;
      if (const uint64_t unmarked = DifferingChunks(bytes, set.shadow)) {
        NoteUnmarked(bytes, set.shadow, unmarked, kind, check_period == 1, tags);
        CopyChunks(bytes, set.shadow, unmarked);
        changed |= unmarked;
        fields |= WholeChunkFields(unmarked);  // banjo-nx (2026-10-26): as a whole (its words are gone)
      }
    }
  } else {
    changed = fast_chunks ? ChangedChunks(bytes, set.shadow) : (ChangedChunksReference(bytes, set.shadow), kAllChunks);
  }
  NoteSetWordsOf(buffer, set, bytes, first ? kAllChunks : changed);  // blur-nx (2026-11-13): the changed chunks' blocks
  TagCounts &counts = set.counts;
  static const bool census = REXCVAR_GET(banjo_renut_diag_fast_census);
  if (census && from_marks) {
    const uint64_t inline_only = changed & ~setter_marked;
    const uint64_t by_setter = changed & setter_marked;
    counts.inline_changed += uint64_t(std::popcount(inline_only));
    counts.setter_changed += uint64_t(std::popcount(by_setter));
    counts.touch_marks += uint64_t(std::popcount(setter_marked & kAllChunks));
    counts.tag_only_marks += uint64_t(std::popcount(tag_chunks & ~setter_marked & kAllChunks));
    ++counts.census_ops;
    for (uint64_t left = inline_only; left; left &= left - 1) {
      for (u32 f = WholeChunkFields(left & (~left + 1)); f; f &= f - 1)
        ++counts.inline_fields[std::countr_zero(f)];
    }
    for (uint64_t left = by_setter; left; left &= left - 1) {
      for (u32 f = WholeChunkFields(left & (~left + 1)); f; f &= f - 1)
        ++counts.setter_fields[std::countr_zero(f)];
    }
  }
  ++counts.ops;
  counts.marked_ops += from_marks;
  counts.marked += from_marks ? uint64_t(std::popcount(marked & kAllChunks)) : 0;
  counts.changed += uint64_t(std::popcount(changed));
  if (counts.ops >= kTagFlushOps)
    FlushTagCounts(counts);
  op.plain_dirty = first ? kAllChunks : changed;
  op.fields = first ? kFieldsAll : fields | WholeChunkFields(changed & ~kPlainFields.mixed);
  // banjo-nx (2026-10-28, host packets): the packet key over what this op copies (CopyState below takes the same chunks
  // from the same bytes), so it is the key of the state the replay rebuilds for this op; only the packet words
  // (kPacketWords: the chunks without any are not hashed).
  static const bool state_keys = HostPacketKeysOn();
  if (state_keys) {
    for (uint64_t bits = op.plain_dirty & kPacketWords.chunks; bits; bits &= bits - 1) {
      const u32 i = u32(std::countr_zero(bits));
      const uint64_t part = PacketChunkPart(bytes, i);
      set.state_key ^= set.chunk_key[i] ^ part;
      set.chunk_key[i] = part;
    }
    op.call.state_key = set.state_key ? set.state_key : 1;
  }
  // banjo-nx (2026-10-28, banjo_renut_first_op_groups / A/B 51): the first op's groups, those set so far (see the key).
  static const bool first_groups_key = REXCVAR_GET(banjo_renut_first_op_groups);
  const bool first_groups = first && first_groups_key && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 51;
  op.vs_dirty = first ? (first_groups ? set.vs : ~uint64_t(0)) : vs_dirty;
  op.ps_dirty = first ? (first_groups ? set.ps : ~uint64_t(0)) : ps_dirty;
  // banjo-nx (2026-10-28, per-draw night): the resources planned first (their table lines in flight over the copy).
  // blur-nx (2026-11-18, the Renut host batch's #1 and #2, blur_renut_record_skip_unused / A/B 230): a replay with inherited
  // words (banjo_renut_replay_inherits, and not ultimate_renut_replay_inherit_same's case) reads neither the resource
  // objects nor the slot extents (Replay: use_resources and g_draw_slot_extents need !inherits), and an op without them
  // replays as one recorded with the keys off. Blur: every replay inherits (0 of 131,072 as recorded), yet each op
  // planned and made its objects (FinishResources 0.44 ms a frame, the lookups' 2,000 a frame) and its extents
  // (RecordExtents 0.39 ms), and the render thread applied and prefetched them.
  static const bool replays_inherit = REXCVAR_GET(banjo_renut_replay_inherits);
  const bool skip_unused = replays_inherit && ::renut::gpu::host_batch::On(::renut::gpu::host_batch::kSkipUnused);
  static const bool resource_objects_key = ResourceObjectsOn();
  const bool resource_objects = resource_objects_key && !skip_unused;
  ResourcePlan plan;
  if (resource_objects)
    PlanResources(plan, bytes, op.plain_dirty, kind, touch_slot);
  op.state = CopyState(buffer, bytes, op.plain_dirty, op.vs_dirty, op.ps_dirty);
  // banjo-nx (2026-10-27): the changed texture slots' extents (banjo_renut_record_extents).
  if (RecordExtentsOn() && !skip_unused)
    op.extents = RecordExtents(buffer, bytes, op.plain_dirty, touch_slot);
  // banjo-nx (2026-10-27, Phase A): the op's resources (banjo_renut_resource_objects).
  if (resource_objects)
    op.resources = FinishResources(buffer, plan);
  *out_buffer = &buffer;
  return &op;
}

void CopyPending(const PendingVertices &p) {
  auto it = g_buffers.find(p.command_buffer);
  if (it == g_buffers.end() || !it->second || p.offset + p.size > it->second->arena.size())
    return;
  std::memcpy(it->second->arena.data() + p.offset, p.source, p.size);
}

// Copies pending vertex data selected by 'match' into its buffer. Requires
// g_record_mutex.
template <typename Match>
void FlushPending(Match match) {
  for (size_t i = 0; i < g_pending_vertices.size();) {
    if (match(g_pending_vertices[i])) {
      CopyPending(g_pending_vertices[i]);
      g_recording_devices[g_pending_vertices[i].device].pending.fetch_sub(1, std::memory_order_acq_rel);  // blur-nx
      PendingOf(g_pending_vertices[i].device).fetch_sub(1, std::memory_order_acq_rel);
      g_pending_vertices[i] = g_pending_vertices.back();
      g_pending_vertices.pop_back();
    } else {
      ++i;
    }
  }
  SyncPendingCount();
}

void FlushDevicePending(u32 device_va) {
  if (g_pending_vertices.empty())
    return;
  FlushPending([device_va](const PendingVertices &p) { return p.device == device_va; });
}

void MergeInherited(u8 *state, const u8 *running, uint64_t set, u32 offset) {
  if (set == ~uint64_t(0))
    return;
  for (u32 group = 0; group < 64; ++group) {
    if (!(set & (uint64_t(1) << (63 - group)))) {
      const u32 at = offset + group * kConstantGroupBytes;
      std::memcpy(state + at, running + at, kConstantGroupBytes);
    }
  }
}

// The tile predication mask of a device or snapshot; ~0 when none is set.
u32 Predication(const u8 *state) {
  const u32 mask = *reinterpret_cast<const be_u32 *>(state + d3d::kDeviceOffsetPredication);
  return mask ? mask : ~0u;
}

// banjo-nx (2026-10-23, render-thread round): a device word of the op after this one as it will be once ApplyState has
// brought the state to it: from that op's chunks when it changed the word's chunk (they are in the arena in chunk
// order, all whole but the tail's last), else from 'state', this op's. Words outside the chunks (the float constants
// and what comes before the fetch constants) are read from 'state'. Only for prefetching.
u32 PeekNextWord(const u8 *state, const u8 *next, uint64_t next_dirty, u32 offset) {
  u32 chunk = UINT32_MAX;
  if (offset >= kTailBegin)
    chunk = kFetchChunks + (offset - kTailBegin) / kChunkBytes;
  else if (offset >= kFetchBegin && offset < kFetchBegin + kFetchBytes)
    chunk = (offset - kFetchBegin) / kChunkBytes;
  const u8 *at = chunk != UINT32_MAX && ((next_dirty >> chunk) & 1)
                     ? next + std::popcount(next_dirty & ((uint64_t(1) << chunk) - 1)) * kChunkBytes +
                           (offset - ChunkOffset(chunk))
                     : state + offset;
  u32 word;
  std::memcpy(&word, at, 4);
  return __builtin_bswap32(word);
}
static_assert(d3d::kDeviceOffsetVertexShader >= kTailBegin && d3d::kDeviceOffsetPixelShader >= kTailBegin);

// banjo-nx (2026-10-23, render-thread round): the guest headers the next draw reads that it changed: its textures',
// vertex buffers' and index buffer's. Second form (same day, after the console profile gave the first 5 % of the render
// thread: 43 words tested at every draw): only the tail chunks that hold header words and that the op changed, and in
// them only the words whose value is not this draw's (this draw read those headers itself: they are cached).
struct HeaderChunk {
  u8 count = 0;
  u8 word[16];    // the word's offset in the chunk / 4
  u16 bytes[16];  // the header it names
};
struct HeaderChunks {
  uint64_t mask = 0;  // tail chunks (bit c = chunk kFetchChunks + c of the op's plain_dirty) holding header words
  HeaderChunk chunk[64];
};
constexpr HeaderChunks MakeHeaderChunks() {
  HeaderChunks h{};
  auto add = [&](u32 offset, u32 bytes) {
    const u32 c = (offset - kTailBegin) / kChunkBytes;
    HeaderChunk &k = h.chunk[c];
    k.word[k.count] = u8((offset - kTailBegin - c * kChunkBytes) / 4);
    k.bytes[k.count] = u16(bytes);
    ++k.count;
    h.mask |= uint64_t(1) << c;
  };
  for (u32 r = 0; r < d3d::kTextureFetchSlots; ++r)
    add(d3d::kDeviceOffsetTextures + r * 4, sizeof(d3d::BaseTexture));
  for (u32 stream = 0; stream < 16; ++stream)
    add(d3d::kDeviceOffsetStreamSources + stream * 4, sizeof(d3d::VertexBuffer));
  add(d3d::kDeviceOffsetIndexBuffer, sizeof(d3d::IndexBuffer));
  return h;
}
constexpr HeaderChunks kHeaderChunks = MakeHeaderChunks();
static_assert(kHeaderChunks.mask >> kTailChunks == 0, "header words past the tail chunks");

// banjo-nx (2026-10-26, the record-time draw path's round 3): the next draw's texture memo entries, for the slots this
// draw bound (FastTextureMaskGuess), each from the next op's texture word and fetch constant (its chunks where it
// changed them, else this draw's): FillTextures' lookups of the next draw, in flight while this one is drawn.
// banjo-nx (2026-10-27, render thread night): the bit of an op's plain_dirty for the chunk holding a device byte (as
// PeekNextWord finds it), 0 outside the chunks.
constexpr uint64_t ChunkBitOf(u32 offset) {
  if (offset >= kTailBegin)
    return uint64_t(1) << (kFetchChunks + (offset - kTailBegin) / kChunkBytes);
  if (offset >= kFetchBegin && offset < kFetchBegin + kFetchBytes)
    return uint64_t(1) << ((offset - kFetchBegin) / kChunkBytes);
  return 0;
}

void PrefetchNextTextures(const u8 *state, const u8 *next, uint64_t next_dirty) {
  u32 mask = FastTextureMaskGuess() & ((1u << d3d::kTextureFetchSlots) - 1u);
  for (; mask; mask &= mask - 1) {
    const u32 r = u32(std::countr_zero(mask));
    const u32 at = u32(offsetof(d3d::Device, m_Constants) + offsetof(d3d::Constants, TextureFetch)) +
                   r * u32(sizeof(d3d::TextureFetchConstant));
    // banjo-nx (2026-10-27, render thread night): a slot whose texture word and fetch constant the next op leaves as
    // they are has this draw's memo entry, which this draw's lookup brings in: nothing to fetch (seven words peeked
    // and an entry hashed for it at every draw before).
    if (!(next_dirty & (ChunkBitOf(d3d::kDeviceOffsetTextures + r * 4) | ChunkBitOf(at) |
                        ChunkBitOf(at + u32(sizeof(d3d::TextureFetchConstant)) - 1))))
      continue;
    const u32 va = PeekNextWord(state, next, next_dirty, d3d::kDeviceOffsetTextures + r * 4);
    if (!va)
      continue;
    // The fetch constant's six words as stored (big-endian), from the chunk that holds each.
    alignas(4) u8 raw[sizeof(d3d::TextureFetchConstant)];
    for (u32 w = 0; w < sizeof(raw) / 4; ++w) {
      const u32 word = __builtin_bswap32(PeekNextWord(state, next, next_dirty, at + w * 4));
      std::memcpy(raw + w * 4, &word, 4);
    }
    d3d::TextureFetchConstant fetch;
    std::memcpy(&fetch, raw, sizeof(fetch));
    PrefetchTextureMemoLocked(va, fetch);
  }
}

void PrefetchNextHeaders(const u8 *state, const u8 *next, uint64_t next_dirty) {
  uint64_t chunks = (next_dirty >> kFetchChunks) & kHeaderChunks.mask;
  if (!chunks)
    return;
  // The op's chunks are in the arena in chunk order: chunk i is at popcount(dirty below i) * 64.
  for (; chunks; chunks &= chunks - 1) {
    const u32 c = u32(std::countr_zero(chunks));
    const u32 i = kFetchChunks + c;
    const u8 *now = next + std::popcount(next_dirty & ((uint64_t(1) << i) - 1)) * kChunkBytes;
    const u8 *was = state + kTailBegin + c * kChunkBytes;
    const HeaderChunk &k = kHeaderChunks.chunk[c];
    for (u32 w = 0; w < k.count; ++w) {
      u32 word, old;
      std::memcpy(&word, now + 4u * k.word[w], 4);
      std::memcpy(&old, was + 4u * k.word[w], 4);
      if (word == old || !word)
        continue;
      if (const auto *p = GuestPtr<const u8>(__builtin_bswap32(word))) {
        __builtin_prefetch(p);
        __builtin_prefetch(p + k.bytes[w] - 1);
      }
    }
  }
}
static_assert(d3d::kDeviceOffsetIndexBuffer >= kTailBegin && d3d::kDeviceOffsetStreamSources >= kTailBegin &&
              d3d::kDeviceOffsetTextures >= kTailBegin);

// banjo-nx (2026-10-27, banjo_renut_shadow_sort): PrefetchNextHeaders for a sorted entry: the guest headers named in
// the tail chunks it brings (their new bytes are the arena copies the pairs point at).
void PrefetchSortedHeaders(const u8 *arena, const SortPair *pairs, u32 count) {
  for (u32 p = 0; p < count; ++p) {
    const u32 slot = pairs[p].slot;
    if (slot < kFetchChunks || slot >= 64)
      continue;
    const u32 c = slot - kFetchChunks;
    if (!((kHeaderChunks.mask >> c) & 1))
      continue;
    const u8 *now = arena + pairs[p].offset;
    const HeaderChunk &k = kHeaderChunks.chunk[c];
    for (u32 w = 0; w < k.count; ++w) {
      u32 word;
      std::memcpy(&word, now + 4u * k.word[w], 4);
      if (!word)
        continue;
      if (const auto *ptr = GuestPtr<const u8>(__builtin_bswap32(word))) {
        __builtin_prefetch(ptr);
        __builtin_prefetch(ptr + k.bytes[w] - 1);
      }
    }
  }
}

// The device each Replay depth rebuilds its ops' states in (ApplyState); only
// the render thread replays. The bytes before the fetch constants stay zero.
alignas(64) u8 g_replay_state[kMaxRunDepth][d3d::kDeviceSnapshotSize];

// banjo-nx (2026-10-26, banjo_renut_diag_fast_census): whether a replayed recording is the one its command buffer had
// the frame before: its ops and their device chunks (the structure), and its float constants. Diagnostics only.
struct RepeatCensus {
  struct Last {
    uint64_t structure = 0, constants = 0;
    // banjo-nx (2026-10-26, setter rewrite): each op's own hash (its kind, call and device chunks, no constants) and its
    // chunks' hashes by field, the frame before.
    std::vector<uint64_t> ops;
    std::vector<std::array<uint64_t, 18>> fields;
  };
  std::unordered_map<u32, Last> last;
  uint64_t recordings = 0, draws = 0, same_structure = 0, same_structure_draws = 0, same_all = 0, same_all_draws = 0;
  // banjo-nx (2026-10-26, setter rewrite): draws whose op is the one at the same place in the recording the frame before,
  // those whose every op up to theirs is, and for the others the fields whose bytes differ.
  uint64_t op_same_draws = 0, prefix_same_draws = 0, compared_draws = 0;
  uint64_t differ_fields[18] = {};
} g_repeat_census;

void CensusRecording(const HostCommandBuffer &buffer, u32 command_buffer_va) {
  uint64_t hs = 0x9E3779B97F4A7C15ull, hc = 0x632BE59BD9B4E019ull;
  const auto mix = [](uint64_t &h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xFF51AFD7ED558CCDull;
  };
  uint64_t draws = 0;
  RepeatCensus &census = g_repeat_census;
  RepeatCensus::Last &previous = census.last[command_buffer_va];
  std::vector<uint64_t> op_hashes;
  std::vector<std::array<uint64_t, 18>> op_fields;
  op_hashes.reserve(buffer.ops.size());
  op_fields.reserve(buffer.ops.size());
  bool prefix = true;
  for (const Op &op : buffer.ops) {
    uint64_t ho = 0x2545F4914F6CDD1Dull;
    std::array<uint64_t, 18> fh{};
    mix(ho, uint64_t(op.kind) | uint64_t(op.plain_dirty) << 8);
    if (op.kind == OpKind::Draw) {
      const OpCall &c = op.call;
      mix(ho, uint64_t(c.primitive) | uint64_t(c.indexed) << 8 | uint64_t(u32(c.base_vertex)) << 16);
      mix(ho, uint64_t(c.start_index) | uint64_t(c.start_vertex) << 32);
      mix(ho, c.count);
    } else if (op.kind == OpKind::Run) {
      mix(ho, op.command_buffer);
    }
    if (op.state != kNone) {
      const u8 *at = buffer.arena.data() + op.state;
      for (uint64_t chunks = op.plain_dirty; chunks; chunks &= chunks - 1) {
        const u32 i = u32(std::countr_zero(chunks));
        const u32 size = ChunkSize(i);
        if (size_t(at - buffer.arena.data()) + size > buffer.arena.size())
          break;
        const uint64_t h = banjo::renut_gpu::ContentHash64(at, size);
        mix(ho, h);
        for (u32 f = kPlainFields.chunk[i]; f; f &= f - 1)
          mix(fh[std::countr_zero(f)], h ^ i);
        at += size;
      }
    }
    const size_t k = op_hashes.size();
    const bool same = k < previous.ops.size() && previous.ops[k] == ho;
    prefix = prefix && same;
    if (op.kind == OpKind::Draw && !previous.ops.empty()) {
      ++census.compared_draws;
      census.op_same_draws += same;
      census.prefix_same_draws += prefix;
      if (!same && k < previous.fields.size()) {
        for (u32 f = 0; f < 18; ++f)
          census.differ_fields[f] += fh[f] != previous.fields[k][f];
      }
    }
    op_hashes.push_back(ho);
    op_fields.push_back(fh);
    mix(hs, uint64_t(op.kind) | uint64_t(op.fields) << 8);
    mix(hs, op.plain_dirty);
    mix(hs, op.vs_set ^ (op.ps_set << 1));
    mix(hs, op.vs_dirty ^ (op.ps_dirty << 1));
    if (op.kind == OpKind::Draw) {
      ++draws;
      const OpCall &c = op.call;
      mix(hs, uint64_t(c.primitive) | uint64_t(c.indexed) << 8 | uint64_t(c.tessellation) << 9 |
                  uint64_t(u32(c.base_vertex)) << 16);
      mix(hs, uint64_t(c.start_index) | uint64_t(c.start_vertex) << 32);
      mix(hs, c.count);
    } else if (op.kind == OpKind::Run) {
      mix(hs, op.command_buffer);
    } else {
      mix(hs, uint64_t(op.flags) | uint64_t(op.dest_texture) << 32);
    }
    if (op.state == kNone)
      continue;
    size_t plain = 0;
    for (uint64_t chunks = op.plain_dirty; chunks; chunks &= chunks - 1)
      plain += ChunkSize(u32(std::countr_zero(chunks)));
    const size_t constants = size_t(std::popcount(op.vs_dirty) + std::popcount(op.ps_dirty)) * kConstantGroupBytes;
    const u8 *at = buffer.arena.data() + op.state;
    if (op.state + plain + constants > buffer.arena.size())
      continue;
    mix(hs, banjo::renut_gpu::ContentHash64(at, plain));
    if (constants)
      mix(hc, banjo::renut_gpu::ContentHash64(at + plain, constants));
  }
  RepeatCensus &c = g_repeat_census;
  ++c.recordings;
  c.draws += draws;
  RepeatCensus::Last &l = previous;
  l.ops = std::move(op_hashes);
  l.fields = std::move(op_fields);
  if (l.structure == hs) {
    ++c.same_structure;
    c.same_structure_draws += draws;
    if (l.constants == hc) {
      ++c.same_all;
      c.same_all_draws += draws;
    }
  }
  l.structure = hs;
  l.constants = hc;
}

// banjo-nx (2026-10-28, the host packets census, banjo_renut_diag_packet_census): how many replayed draws a cache of
// derived draw state keyed by the draw's whole device state would answer: the fetch constants and the tail (every byte
// a draw reads but the float constants, which are pushed at every draw anyway), with the primitive and whether it is
// indexed. "Derive" draws are those whose chain fields since the draw before are not empty: the lean executor
// re-derives something for them today, and only those gain from a packet. Diagnostics only (3 KB hashed a draw).
struct PacketCensus {
  struct Slot {
    uint64_t key = 0;
  };
  static constexpr u32 kBits = 13;  // the table a packet cache could afford: 8,192 entries, direct-mapped
  Slot table[1u << kBits];
  std::unordered_map<uint64_t, uint64_t> seen;  // key -> the frame it was last drawn in (unbounded)
  uint64_t draws = 0, derive = 0, hit = 0, derive_hit = 0, seen_recent = 0, derive_seen_recent = 0;
  uint64_t derive_shaders = 0, derive_shaders_hit = 0, derive_textures = 0, derive_textures_hit = 0;
  uint64_t derive_geometry = 0, derive_geometry_hit = 0, keys_new = 0;
  // The recording's state keys (PacketKeyOf) against the whole hash: one key with two states would be a wrong key (a
  // packet answering another state); one state with two keys only a key more precise than it needs to be.
  std::unordered_map<uint64_t, uint64_t> key_state, state_key;
  uint64_t keyed = 0, unkeyed = 0, key_two_states = 0, state_two_keys = 0;
  // banjo-nx (2026-10-28, second census): which field makes a draw's state new from frame to frame. For each of the 18
  // fields, a key without its chunks and a key of its chunks alone, each looked up in a table of the frames keys were
  // last drawn in (direct-mapped, approximate): "recent" = drawn this frame or the one before.
  static constexpr u32 kFieldCount = 18, kStampBits = 16;
  struct Stamp {
    uint64_t key = 0, frame = 0;
  };
  std::vector<Stamp> without_stamps = std::vector<Stamp>(size_t(kFieldCount) << kStampBits);
  std::vector<Stamp> alone_stamps = std::vector<Stamp>(size_t(kFieldCount) << kStampBits);
  uint64_t without_recent[kFieldCount] = {}, alone_recent[kFieldCount] = {};
};
inline bool StampRecent(std::vector<PacketCensus::Stamp> &t, u32 field, uint64_t key, uint64_t frame) {
  PacketCensus::Stamp &s =
      t[(size_t(field) << PacketCensus::kStampBits) | size_t((key * 0xFF51AFD7ED558CCDull) >> (64 - PacketCensus::kStampBits))];
  const bool recent = s.key == key && s.frame + 1 >= frame;
  s.key = key;
  s.frame = frame;
  return recent;
}
PacketCensus *g_packet_census = nullptr;

void CensusPacket(const u8 *state, const OpCall &call, u32 fields) {
  if (!g_packet_census)
    g_packet_census = new PacketCensus();
  PacketCensus &c = *g_packet_census;
  const uint64_t frame = TextureFrameLocked();
  uint64_t key = banjo::renut_gpu::ContentHash64(state + kFetchBegin, kFetchBytes);
  key = (key ^ banjo::renut_gpu::ContentHash64(state + kTailBegin, kTailBytes)) * 0x9E3779B97F4A7C15ull;
  key ^= (uint64_t(call.primitive) | uint64_t(call.indexed) << 8 | uint64_t(call.tessellation) << 9) *
         0xC2B2AE3D27D4EB4Full;
  {
    // banjo-nx (2026-10-28, second census): the keys without each field, and of each field alone.
    uint64_t parts[kPlainChunks];
    uint64_t whole = 0;
    for (u32 i = 0; i < kPlainChunks; ++i) {
      parts[i] = ChunkKeyPart(state, i);
      whole ^= parts[i];
    }
    uint64_t field_part[PacketCensus::kFieldCount] = {};
    for (u32 i = 0; i < kPlainChunks; ++i)
      for (u32 fb = kPlainFields.chunk[i]; fb; fb &= fb - 1)
        if (u32(std::countr_zero(fb)) < PacketCensus::kFieldCount)
          field_part[std::countr_zero(fb)] ^= parts[i];
    const uint64_t call_bits =
        (uint64_t(call.primitive) | uint64_t(call.indexed) << 8 | uint64_t(call.tessellation) << 9) * 0xD6E8FEB86659FD93ull;
    for (u32 fi = 0; fi < PacketCensus::kFieldCount; ++fi) {
      c.without_recent[fi] += StampRecent(c.without_stamps, fi, whole ^ field_part[fi] ^ call_bits, frame);
      c.alone_recent[fi] += StampRecent(c.alone_stamps, fi, field_part[fi] ^ (uint64_t(fi + 1) * 0x9E3779B97F4A7C15ull),
                                        frame);
    }
  }
  if (const uint64_t k = PacketKeyOf(call)) {
    ++c.keyed;
    // banjo-nx (2026-10-28, second form): the packet words of the rebuilt state hashed here must give the recorded key
    // (key_two_states: they did not); state_two_keys counts the whole states that share a key (expected now).
    uint64_t narrow = 0;
    for (uint64_t bits = kPacketWords.chunks; bits; bits &= bits - 1)
      narrow ^= PacketChunkPart(state, u32(std::countr_zero(bits)));
    OpCall expect = call;
    expect.state_key = narrow ? narrow : 1;
    c.key_two_states += PacketKeyOf(expect) != k;
    auto [ks, ks_new] = c.key_state.try_emplace(k, key);
    c.state_two_keys += !ks_new && ks->second != key;
    key = k;  // the table below as a packet cache would see it
  } else {
    ++c.unkeyed;
  }
  constexpr u32 kDerive = kFieldShaders | kFieldDecl | kFieldFetch | kFieldTexWords | kFieldVertexFetch | kFieldBools |
                          kFieldViewPackets | kField29C0 | kFieldTargets | kFieldDest | kFieldPredication |
                          kFieldStreams | kFieldIndex;
  const bool derive = (fields & kDerive) != 0;
  PacketCensus::Slot &slot = c.table[(key * 0xFF51AFD7ED558CCDull) >> (64 - PacketCensus::kBits)];
  const bool hit = slot.key == key;
  slot.key = key;
  auto [it, inserted] = c.seen.try_emplace(key, frame);
  const bool recent = !inserted && it->second + 1 >= frame;  // drawn this frame or the one before
  it->second = frame;
  ++c.draws;
  c.keys_new += inserted;
  c.hit += hit;
  c.seen_recent += recent;
  if (derive) {
    ++c.derive;
    c.derive_hit += hit;
    c.derive_seen_recent += recent;
  }
  if (fields & (kFieldShaders | kFieldDecl)) {
    ++c.derive_shaders;
    c.derive_shaders_hit += hit;
  }
  if (fields & (kFieldFetch | kFieldTexWords)) {
    ++c.derive_textures;
    c.derive_textures_hit += hit;
  }
  if (fields & (kFieldStreams | kFieldIndex | kFieldVertexFetch)) {
    ++c.derive_geometry;
    c.derive_geometry_hit += hit;
  }
}

// On the render thread. 'predication': the mask the buffer runs under
// (SetPredication before RunCommandBuffer), which the draws' own masks narrow
// (see SetReplayPredicationLocked).
void Replay(const u8 *running, const BufferSet &buffers, u32 command_buffer_va, u32 depth,
            u32 predication) {
  if (depth >= kMaxRunDepth)
    return;
  auto it = buffers.find(command_buffer_va);
  if (it == buffers.end() || !it->second)
    return;
  // Only this thread touches a captured buffer: the game records into a new
  // one (BeginRecording).
  HostCommandBuffer &buffer = *it->second;
  // halo3-nx (2026-11-03, banjo_renut_replay_inherits): see BuildInheritRuns.
  static const bool inherits_on = REXCVAR_GET(banjo_renut_replay_inherits);
  if (inherits_on && !buffer.inherit_built)
    BuildInheritRuns(buffer);
  // renut-ultimate (audit, 2026-11-07, ultimate_renut_replay_inherit_same): the running device holding every inherited word
  // as recorded is the recorded device: no copies, and the record's answers below hold.
  const bool inherit_same = inherits_on && InheritedAsRecorded(buffer, running);
  const bool inherits = inherits_on && !inherit_same;
  // blur-nx (2026-11-18, the Renut host batch's #3, blur_renut_inherit_by_chunk / A/B 231): see the op loop.
  const bool inherit_by_chunk = inherits && buffer.inherit_chunk_ready &&
                                ::renut::gpu::host_batch::On(::renut::gpu::host_batch::kInheritByChunk);
  const bool hb_census = ::renut::gpu::host_batch::CensusOn();
  if (inherits_on) {
    ++g_inherit_replays;
    g_inherit_same_replays += inherit_same;
    if ((g_inherit_replays & 0xFFFF) == 0)
      RGPU_INFO("[renut] replays with the inherited words as recorded (ultimate_renut_replay_inherit_same): {} of {}",
                g_inherit_same_replays, g_inherit_replays);
  }
  static const bool repeat_census = REXCVAR_GET(banjo_renut_diag_fast_census);
  if (repeat_census)
    CensusRecording(buffer, command_buffer_va);
  static uint64_t replays = 0;
  const uint64_t replay = ++replays;
  uint64_t vs_dirty = ~uint64_t(0), ps_dirty = ~uint64_t(0);
  u8 *state = g_replay_state[depth];
  // banjo-nx (2026-10-19, CPU proposal 18): the draws' predication set once for the buffer, not around every draw:
  // only draws read it (ComputeViewport), and a nested run puts this one back when it ends.
  const u32 previous_predication = SetReplayPredicationLocked(predication);
  // banjo-nx (2026-10-20, banjo_renut_chain_reuse_replays): this replay's last draw's hint number and the chunks its ops
  // applied since then (draw.h ChainHint). A nested run's draws take numbers too, so the draw after it marks every chunk.
  static const bool hints = REXCVAR_GET(banjo_renut_chain_reuse_replays);
  uint64_t hint_last = 0, hint_plain = 0;
  u32 hint_fields = 0;  // banjo-nx (2026-10-26)
  const Op *const ops = buffer.ops.data();
  const size_t op_count = buffer.ops.size();
  uint64_t replayed_draws = 0;
  // banjo-nx (2026-10-27, banjo_renut_record_extents): the depth's slot extents, unknown until the ops give them (the
  // first op of a buffer gives all).
  DrawSlotExtents &extents = g_replay_extents[depth];
  extents.mask = 0;
  // banjo-nx (2026-10-27, Phase A): the depth's resources, unknown until the ops give them (the first gives all), and
  // not while A/B 45 has them off.
  DrawResources &resources = g_replay_resources[depth];
  resources.mask = 0;
  resources.ib_valid = false;
  resources.vb_mask = 0;
  resources.obj_mask = 0;  // (geometry objects)
  resources.index_obj = nullptr;
  // halo3-nx: not with inherited state: these answers were worked out from the words the recording saw.
  const bool use_resources =
      !inherits && ResourceObjectsOn() && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 45;
  const bool stream = ReplayStreamOn();  // banjo-nx (2026-10-27, the replay stream)
  // banjo-nx (2026-10-28, banjo_renut_replay_group_pointers / A/B 52): the depth's group pointers (each op sets those it
  // writes before a draw reads them: a set group was written by an op at or before it), or null (copied into the image).
  const u8 **groups = GroupPointersOn() ? g_replay_groups[depth] : nullptr;
  static const bool group_verify = REXCVAR_GET(banjo_renut_replay_group_pointers_verify);
  g_group_pointers_verify = groups && group_verify;
  const bool stream_shaders = stream && ReplayStreamShadersOn();
  // banjo-nx (2026-10-28, per-draw night): the A/B half once a replay (targets 55-57: one look-ahead stage off each).
  const int prefetch_ab = banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed);
  // banjo-nx (2026-10-28): the record prefetch below is OFF by default: it HURT on the console (A/B 57, night_final:
  // render thread +0.99 ms a frame with it on in the static city); banjo_renut_replay_record_prefetch puts it back.
  static const bool record_prefetch = REXCVAR_GET(banjo_renut_replay_record_prefetch);
  static const bool kinds = DrawKindsCensusOn();  // banjo-nx (2026-10-27): the draw-kind census's times
  const Op *merge_prev = nullptr;  // banjo-nx (2026-10-27): the draw op before, with nothing between (the merge census)
  // banjo-nx (2026-10-27, banjo_renut_shadow_sort): the buffer's sorted runs (PlanSortedRuns), each taken at its first op
  // when its shaders allow any order and A/B target 40 is not in its off half; the ops as recorded otherwise.
  const bool sort_runs = !inherits && ShadowSortOn() && buffer.sort_state.load(std::memory_order_acquire) == 2 &&
                         !buffer.sort_runs.empty() &&
                         banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 40;
  size_t sort_cursor = 0;
  for (size_t op_index = 0; op_index < op_count; ++op_index) {
    if (sort_runs && sort_cursor < buffer.sort_runs.size()) {
      while (sort_cursor < buffer.sort_runs.size() && buffer.sort_runs[sort_cursor].begin < op_index)
        ++sort_cursor;  // (a merged draw took a run's first op)
      if (sort_cursor < buffer.sort_runs.size() && buffer.sort_runs[sort_cursor].begin == op_index) {
        const SortRun &run = buffer.sort_runs[sort_cursor++];
        if (SortedRunShadersOk(buffer, run)) {
          const u8 *arena = buffer.arena.data();
          const SortEntry *entries = buffer.sort_entries.data() + run.first_entry;
          // banjo_renut_shadow_sort_verify: each op's device as recorded (the run applied in order from this one).
          static const bool sort_verify = REXCVAR_GET(banjo_renut_shadow_sort_verify);
          static std::vector<u8> verify_states;
          constexpr u32 kVerifyFrom = kFetchBegin, kVerifyBytes = d3d::kDeviceSnapshotSize - kFetchBegin;
          if (sort_verify) {
            static std::vector<u8> scratch;
            scratch.assign(state, state + d3d::kDeviceSnapshotSize);
            verify_states.resize(size_t(run.end - run.begin) * kVerifyBytes);
            for (u32 j = run.begin; j < run.end; ++j) {
              ApplyState(scratch.data(), arena + ops[j].state, ops[j].plain_dirty, ops[j].vs_dirty, ops[j].ps_dirty);
              std::memcpy(verify_states.data() + size_t(j - run.begin) * kVerifyBytes, scratch.data() + kVerifyFrom,
                          kVerifyBytes);
            }
          }
          for (u32 k = 0; k < run.entry_count; ++k) {
            const SortEntry &e = entries[k];
            const uint64_t entry_start = kinds ? RenderTicks() : 0;
            const SortPair *pairs = buffer.sort_pairs.data() + e.first_pair;
            for (u32 p = 0; p < e.pair_count; ++p)
              ApplySortPair(state, arena, pairs[p], groups);
            hint_plain |= e.plain;
            hint_fields |= e.fields;
            vs_dirty |= e.vs_dirty;
            ps_dirty |= e.ps_dirty;
            if (sort_verify) {
              const u32 as = e.op == kSortRestore ? run.end - 1 : e.op;
              const u8 *want = verify_states.data() + size_t(as - run.begin) * kVerifyBytes;
              ++g_sort_verified;
              if (std::memcmp(state + kVerifyFrom, want, kVerifyBytes) != 0) {
                ++g_sort_verify_differ;
                u32 at = 0;
                while (at < kVerifyBytes && state[kVerifyFrom + at] == want[at])
                  ++at;
                static uint32_t logged = 0;
                if (logged++ < 16)
                  RGPU_WARN("banjo_renut_shadow_sort_verify: the device of op {} of a sorted run [{}, {}) differs from "
                            "the recorded one at +0x{:X}{}", as, run.begin, run.end, kVerifyFrom + at,
                            e.op == kSortRestore ? " (the run's end)" : "");
              }
            }
            if (e.op == kSortRestore)
              continue;
            const Op &sop = ops[e.op];
            // The next draw's shader lookups' lines, when its shaders are others, and the guest headers its chunks name
            // (the replay's look-ahead, A/B 29).
            if (k + 2 < run.entry_count && ReplayLookAheadOn()) {
              const SortEntry &ne = entries[k + 1];
              const Op &nop = ops[ne.op];
              if (nop.sort_vs != sop.sort_vs)
                PrefetchShaderLocked(nop.sort_vs, RecordKind::VertexShader);
              if (nop.sort_ps != sop.sort_ps)
                PrefetchShaderLocked(nop.sort_ps, RecordKind::PixelShader);
              PrefetchSortedHeaders(arena, buffer.sort_pairs.data() + ne.first_pair, ne.pair_count);
            }
            const ReplayConstants constants{running, sop.vs_set, sop.ps_set, &vs_dirty, &ps_dirty, replay, groups};
            ++replayed_draws;
            if (hints) {
              const uint64_t seq = NextChainSeq();
              g_chain_hint.seq = seq;
              if (hint_last && seq == hint_last + 1) {
                PlainToChain(hint_plain, g_chain_hint.changed);
                g_chain_hint.fields = hint_fields;
              } else {
                g_chain_hint.changed[0] = g_chain_hint.changed[1] = ~uint64_t(0);
                g_chain_hint.fields = kFieldsAll;
              }
              hint_last = seq;
              hint_plain = 0;
              hint_fields = 0;
            }
            SetReplayStackHeaders(StackHeadersOf(buffer, e.op));  // blur-nx (2026-11-10, port 76; its key 215)
            DrawStateLocked(state, CallOf(sop.call, arena), &constants);
            SetReplayStackHeaders(nullptr);
            g_chain_hint = {};
            if (kinds)
              NoteDrawKind(RenderTicks() - entry_start);
          }
          ++g_sort_replayed_runs;
          g_sort_replayed_draws += run.entry_count - 1;
          extents.mask = 0;  // banjo-nx (2026-10-27): the sorted run's chunks came without their ops' extents
          resources.mask = 0;  // (and their resources)
          resources.ib_valid = false;
          resources.vb_mask = 0;
          resources.obj_mask = 0;
          resources.index_obj = nullptr;
          merge_prev = nullptr;
          op_index = run.end - 1;  // the loop's step goes on at run.end, from the run's last op as recorded
          continue;
        }
        ++g_sort_refused_runs;
      }
    }
    const Op &op = ops[op_index];
    // banjo-nx (2026-10-27, the replay stream, banjo_renut_replay_stream / A/B 46): the recording read as a stream, the
    // way Unleashed's render thread takes its queue: the op two ahead's two lines, and for the next op (its lines came
    // with the previous op) what this loop and its draw will read first: its state in the arena (ApplyState), its
    // resource and extent records, and with other shaders their lookups' lines (recorded words, nothing decoded:
    // the look-ahead's peeks at the device, A/B 29, cost more than they saved). Prefetches only.
    if (stream) {
      // Three stages (2026-10-27, after the b35 profile's shader record misses): the op three ahead's lines; for the op
      // two ahead (its lines came an op ago), the shader memo's sets of shaders it changes to; for the next op (its sets
      // came an op ago), the records they name.
      if (op_index + 3 < op_count) {
        // (not 'far': a macro of the Windows headers, the PC build)
        const char *three_ahead = reinterpret_cast<const char *>(&ops[op_index + 3]);
        __builtin_prefetch(three_ahead);
        __builtin_prefetch(three_ahead + 64);
        // sasrt-nx (2026-11-12, batch C/D, item C7; port 93): the op's other line its replay reads: 'binding'
        // (halo3-nx's recorded binds, tested at every replayed draw). sasrt-nx's other lines (+384 .. +640: its recorded
        // surfaces and index buffer) are not in the base's op. Prefetches only.
        static_assert(offsetof(Op, binding) >= 192 && offsetof(Op, binding) + sizeof(u32) <= 256,
                      "C7 prefetches the line of 'binding'");
        __builtin_prefetch(three_ahead + 192);
      }
      // (Without the shader stages, A/B 49: the next op's sets, as c2 did.)
      const size_t sets_at = stream_shaders && op_index + 2 < op_count ? op_index + 2 : op_index + 1;
      if (sets_at < op_count) {
        const Op &with = ops[sets_at];
        if (with.kind == OpKind::Draw && (with.fields & kFieldShaders)) {
          PrefetchShaderLocked(with.call.vs_word, RecordKind::VertexShader);
          PrefetchShaderLocked(with.call.ps_word, RecordKind::PixelShader);
        }
      }
      // banjo-nx (2026-10-28, ARM audit): the resources a stage further (record two ahead, objects one ahead, copies at
      // the op's own ApplyResources).
      if (op_index + 2 < op_count && ops[op_index + 2].resources != kNoResources) {
        const u8 *r = buffer.arena.data() + ops[op_index + 2].resources;
        __builtin_prefetch(r);
        __builtin_prefetch(r + 64);
      }
      if (op_index + 1 < op_count) {
        const Op &next = ops[op_index + 1];
        const u8 *arena = buffer.arena.data();
        if (next.state != kNone && prefetch_ab != 55) {
          const u32 bytes = u32(std::popcount(next.plain_dirty)) * kChunkBytes +
                            u32(std::popcount(next.vs_dirty) + std::popcount(next.ps_dirty)) * kConstantGroupBytes;
          const u8 *s = arena + next.state;
          for (u32 at = 0; at < std::min(bytes, kStreamStateBytes); at += 64)
            __builtin_prefetch(s + at);
        }
        if (next.resources != kNoResources && prefetch_ab != 56)
          PrefetchResourceObjects(arena + next.resources, resources);
        // banjo-nx (2026-10-28, per-draw night): its shader records' lines, from its words (resources.h), and with a
        // declaration of the device's its record's (a record at its own guest address too: the hash, stream order,
        // used mask and first streams the geometry reads first, a miss at most declaration changes in the d4 profile).
        // (OFF by default: see banjo_renut_replay_record_prefetch; it cost the console's render thread ~1 ms a frame.)
        if (record_prefetch && next.kind == OpKind::Draw && (next.fields & kFieldShaders) && prefetch_ab != 57) {
          PrefetchShaderRecordAt(next.call.vs_word);
          PrefetchShaderRecordAt(next.call.ps_word);
        }
        if (record_prefetch && next.kind == OpKind::Draw && (next.fields & (kFieldShaders | kFieldDecl)) &&
            next.call.decl_word && prefetch_ab != 57)
          PrefetchShaderRecordAt(next.call.decl_word);  // (the same three lines from a record's address)
        if (next.extents != kNoExtents)
          __builtin_prefetch(arena + next.extents);
        if (next.kind == OpKind::Draw && next.call.state_key)
          PrefetchHostPacket(PacketKeyOf(next.call));  // banjo-nx (2026-10-28, host packets)
        if (stream_shaders && next.kind == OpKind::Draw && (next.fields & kFieldShaders)) {
          PrefetchShaderRecordLocked(next.call.vs_word, RecordKind::VertexShader);
          PrefetchShaderRecordLocked(next.call.ps_word, RecordKind::PixelShader);
        }
      }
    }
    // banjo-nx (2026-10-28, per-draw night): this op's records came an op ago: what its shader change reads behind
    // them (the cache entries, the bound declaration), while its state is applied.
    if (stream && op.kind == OpKind::Draw && (op.fields & kFieldShaders)) {
      PrefetchShaderBehind(op.call.vs_word);
      PrefetchShaderBehind(op.call.ps_word);
    }
    const uint64_t op_start = kinds ? RenderTicks() : 0;
    ApplyState(state, buffer.arena.data() + op.state, op.plain_dirty, op.vs_dirty, op.ps_dirty, groups);
    if (inherits && op.plain_dirty) {
      // blur-nx (2026-11-18, #3): ApplyState wrote the op's chunks alone, so the inherited words outside them still hold
      // the running device's values from the restore after an earlier op (the buffer's first op applies every chunk, and
      // a replay that inherits takes no sorted runs): only the applied chunks' inherited runs are put back. The image
      // is the same after each op as with every run copied (up to 3.5 KB at each op that applied a chunk).
      if (inherit_by_chunk) {
        u32 copied = 0;
        for (uint64_t bits = op.plain_dirty & kAllChunks; bits; bits &= bits - 1) {
          const u32 c = u32(std::countr_zero(bits));
          for (u32 k = buffer.inherit_chunk_at[c], end = buffer.inherit_chunk_at[c + 1]; k < end; ++k) {
            const auto &[offset, bytes] = buffer.inherit_chunk_runs[k];
            std::memcpy(state + offset, running + offset, bytes);
            copied += bytes;
          }
        }
        if (hb_census) {
          ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kRestoreOps);
          ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kRestoreBytesAll, buffer.inherit_bytes);
          ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kRestoreBytesChunk, copied);
        }
        if (::renut::gpu::host_batch::VerifyOn()) {  // blur-nx (2026-11-18): every inherited run holds the running device's words
          for (const auto &[offset, bytes] : buffer.inherit_runs) {
            if (std::memcmp(state + offset, running + offset, bytes) != 0) {
              ::renut::gpu::host_batch::VerifyDiffer("#3: an inherited run outside the op's chunks differs from the running device");
              std::memcpy(state + offset, running + offset, bytes);
            }
          }
        }
      } else {
        for (const auto &[offset, bytes] : buffer.inherit_runs)
          std::memcpy(state + offset, running + offset, bytes);
        if (hb_census) {
          ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kRestoreOps);
          ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kRestoreBytesAll, buffer.inherit_bytes);
          ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kRestoreBytesChunk, buffer.inherit_bytes);
        }
      }
    }
    if (op.extents != kNoExtents)
      ApplyExtents(extents, buffer.arena.data() + op.extents);
    else if (op.plain_dirty & ((uint64_t(1) << kFetchChunks) - 1))
      extents.mask &= ~SlotsOfFetchChunks(op.plain_dirty);  // (recorded with the key off: not known)
    if (op.resources != kNoResources) {
      ApplyResources(resources, buffer.arena.data() + op.resources, op.kind == OpKind::Draw, stream && op_index > 0);
    } else {  // nothing recorded: a draw has no index words of its own; slots it changed (key off) are not known
      if (op.kind == OpKind::Draw) {
        resources.ib_valid = false;
        resources.vb_mask = 0;
        resources.obj_mask = 0;
        resources.index_obj = nullptr;
      }
      for (uint64_t bits = op.plain_dirty & kAllChunks; bits; bits &= bits - 1)
        resources.mask &= ~kTextureSlotsOfChunk[std::countr_zero(bits)];
    }
    hint_plain |= op.plain_dirty;
    hint_fields |= op.fields;
    vs_dirty |= op.vs_dirty;
    ps_dirty |= op.ps_dirty;
    switch (op.kind) {
    case OpKind::Draw: {
      // banjo-nx (2026-10-23, batch 5): the op's own call unless it has Begin/Draw*UP data to point at (most draws).
      // banjo-nx (2026-10-27, the replay stream): the op keeps the call without pointers (OpCall).
      const DrawCall op_call = CallOf(op.call, buffer.arena.data());
      const bool has_up = op.call.up_vertices != kNoUp || op.call.up_indices != kNoUp;
      const DrawCall *call_ptr = &op_call;
      // banjo-nx (2026-10-27, render thread night, banjo_renut_merge_draws / A/B 33): the draws right after this one that
      // change no state and write no constant (their ApplyState does nothing) and draw the next indices of the same list
      // with the same base vertex (or the next vertices) become part of this draw: one host draw, the same primitives
      // in the same order. Their ops are skipped.
      static const bool merge_on = MergeDrawsOn();
      size_t merged = 0;
      DrawCall merged_call;
      if (merge_on && !has_up && !op.call.tessellation &&
          (op.call.primitive == 1 || op.call.primitive == 2 || op.call.primitive == 4) &&
          banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 33) {
        const DrawCall &c0 = op_call;
        u32 end = c0.indexed ? c0.start_index + c0.count : c0.start_vertex + c0.count;
        size_t j = op_index + 1;
        for (; j < op_count; ++j) {
          const Op &n = ops[j];
          if (n.kind != OpKind::Draw || n.plain_dirty || n.vs_dirty || n.ps_dirty || n.call.up_vertices != kNoUp ||
              n.call.up_indices != kNoUp)
            break;
          const OpCall &c = n.call;
          if (c.primitive != c0.primitive || c.indexed != c0.indexed || c.tessellation || !c.count ||
              (c0.indexed ? (c.base_vertex != c0.base_vertex || c.start_index != end) : c.start_vertex != end))
            break;
          end += c.count;
        }
        if (j > op_index + 1 && MergeableShadersLocked(state)) {
          merged_call = c0;
          merged_call.count = end - (c0.indexed ? c0.start_index : c0.start_vertex);
          merged = j - op_index - 1;
          call_ptr = &merged_call;
          CountMergedDraws(1, merged);
        }
      }
      const DrawCall &call = *call_ptr;
      // banjo-nx (2026-10-27, the user's shadow-caster culling, banjo_renut_shadow_cull_indices; off by default, not
      // pixel-identical): a small shadow caster is not drawn. Its state stays applied and its changes go to the next
      // draw (the hints and the constants' dirty groups keep gathering until a draw takes them).
      if (ShadowCullIndices() && ShadowCasterCulledLocked(state, call)) {
        merge_prev = nullptr;
        op_index += merged;
        break;
      }
      if (kinds) {
        const bool same_state = merge_prev && op.fields == 0 && op.plain_dirty == 0 && op.vs_dirty == 0 &&
                                op.ps_dirty == 0 && !has_up &&
                                merge_prev->call.primitive == call.primitive && merge_prev->call.indexed == call.indexed &&
                                !call.tessellation &&
                                (call.primitive == 1 || call.primitive == 2 || call.primitive == 4);  // point, line, triangle lists
        const bool contiguous = same_state && (call.indexed ? merge_prev->call.base_vertex == call.base_vertex &&
                                                                  merge_prev->call.start_index + merge_prev->call.count ==
                                                                      call.start_index
                                                            : merge_prev->call.start_vertex + merge_prev->call.count ==
                                                                  call.start_vertex);
        NoteDrawMerge(same_state, contiguous);
        merge_prev = &op;
      }
      // The draw reads its inherited constants from the running device.
      const ReplayConstants constants{running, op.vs_set, op.ps_set, &vs_dirty, &ps_dirty, replay, groups};
      ++replayed_draws;  // banjo-nx (2026-10-23): counted once for the buffer (CountDrawSource), not at every draw
      if (hints) {
        const uint64_t seq = NextChainSeq();
        g_chain_hint.seq = seq;
        if (hint_last && seq == hint_last + 1) {
          PlainToChain(hint_plain, g_chain_hint.changed);
          g_chain_hint.fields = hint_fields;
        } else {
          g_chain_hint.changed[0] = g_chain_hint.changed[1] = ~uint64_t(0);
          g_chain_hint.fields = kFieldsAll;
        }
        hint_last = seq;
        hint_plain = 0;
        hint_fields = 0;
        // banjo-nx (2026-10-28, host packets); halo3-nx: the key is of the recorded state alone, so not with inherited state.
        g_chain_hint.packet_key = inherits ? 0 : PacketKeyOf(op.call);
      }
      // banjo-nx (2026-10-23, render-thread round): the next draw's shader lookups' lines, fetched while this draw runs
      // (the first thing a draw reads; its own prefetches come after them), and the start of its state in the arena.
      // banjo-nx (2026-10-27): OFF by default since the console run of 2026-10-27 (+2.15 ms a frame with it, A/B 29):
      // banjo_renut_replay_lookahead. (The op after the ones this draw took, when it took some.)
      const size_t next_index = op_index + 1 + merged;
      if (next_index < op_count && ops[next_index].kind == OpKind::Draw && ops[next_index].state != kNone &&
          ReplayLookAheadOn()) {
        const Op &next = ops[next_index];
        const u8 *next_state = buffer.arena.data() + next.state;
        __builtin_prefetch(next_state);
        // banjo-nx (2026-10-26, round 4): only when the next op changed a shader word (else this draw's, fetched).
        if (next.fields & kFieldShaders) {
          PrefetchShaderLocked(PeekNextWord(state, next_state, next.plain_dirty, d3d::kDeviceOffsetVertexShader),
                               RecordKind::VertexShader);
          PrefetchShaderLocked(PeekNextWord(state, next_state, next.plain_dirty, d3d::kDeviceOffsetPixelShader),
                               RecordKind::PixelShader);
        }
        // banjo-nx (2026-10-26, Unleashed-style step 1): only when the next op changed a header word (its fields).
        if (next.fields & (kFieldTexWords | kFieldStreams | kFieldIndex))
          PrefetchNextHeaders(state, next_state, next.plain_dirty);
        if (next.fields & (kFieldFetch | kFieldTexWords))  // banjo-nx (2026-10-26, round 3)
          PrefetchNextTextures(state, next_state, next.plain_dirty);
      }
      g_draw_slot_extents = extents.mask && !inherits ? &extents : nullptr;  // banjo-nx (2026-10-27); halo3-nx: see use_resources
      g_draw_resources = use_resources ? &resources : nullptr;  // (Phase A)
      {
        static const bool packet_census = REXCVAR_GET(banjo_renut_diag_packet_census);  // banjo-nx (2026-10-28)
        if (packet_census) [[unlikely]]
          CensusPacket(state, op.call, hints ? g_chain_hint.fields : kFieldsAll);
      }
      {
        // halo3-nx (2026-11-03, banjo_renut_diag_replay_state): the words a replayed draw takes from its recording that
        // differ from the running device's: what the recording overrides, right or wrong.
        static const bool replay_state_diag = REXCVAR_GET(banjo_renut_diag_replay_state);
        static uint32_t replay_state_logged = 0;
        if (replay_state_diag && replay_state_logged < 12) [[unlikely]] {
          ++replay_state_logged;
          std::string words;
          u32 differing = 0;
          for (u32 at = 0x2780; at + 4 <= d3d::kDeviceSnapshotSize; at += 4) {
            if (std::memcmp(state + at, running + at, 4) == 0)
              continue;
            if (++differing <= 60)
              words += fmt::format(" {:04X}:{:02X}{:02X}{:02X}{:02X}/{:02X}{:02X}{:02X}{:02X}", at, state[at], state[at + 1],
                                   state[at + 2], state[at + 3], running[at], running[at + 1], running[at + 2],
                                   running[at + 3]);
          }
          RGPU_INFO("[replay state] buffer {:08X} op {}: {} words differ from the running device (recorded/running):{}",
                    command_buffer_va, op_index, differing, words);
        }
      }
      if (op.binding)
        ApplyBindingLocked(op.binding);  // halo3-nx (2026-11-03)
      SetReplayStackHeaders(StackHeadersOf(buffer, op_index));  // blur-nx (2026-11-10, port 76; its key 215)
      DrawStateLocked(state, call, &constants);
      SetReplayStackHeaders(nullptr);
      g_draw_slot_extents = nullptr;
      g_draw_resources = nullptr;
      g_chain_hint = {};  // only for the draw itself
      if (kinds)
        NoteDrawKind(RenderTicks() - op_start);
      op_index += merged;  // banjo-nx (2026-10-27): the ops this draw took (no state, no constants: nothing to apply)
      break;
    }
    case OpKind::Clear:
      merge_prev = nullptr;
      SetReplayStackHeaders(StackHeadersOf(buffer, op_index));  // blur-nx (2026-11-10, port 76; its key 215)
      ClearStateLocked(state, op.flags, op.has_rect ? &op.rect : nullptr, op.color, op.depth,
                       op.stencil);
      SetReplayStackHeaders(nullptr);
      break;
    case OpKind::Resolve:
      merge_prev = nullptr;
      SetReplayStackHeaders(StackHeadersOf(buffer, op_index));  // blur-nx (2026-11-10, port 76; its key 215)
      ResolveStateLocked(state, op.flags, op.has_rect ? &op.rect : nullptr, op.dest_texture,
                         op.has_dest_format ? &op.dest_format : nullptr, op.dest_x, op.dest_y,
                         op.dest_level, op.dest_slice, op.has_color ? op.color : nullptr,
                         op.depth, op.stencil);
      SetReplayStackHeaders(nullptr);
      break;
    case OpKind::Run:
      // The nested buffer runs on this op's state: complete its constants.
      // Idempotent: the inherited groups are rewritten on every run.
      merge_prev = nullptr;
      if (groups)  // banjo-nx (2026-10-28): the image holds this replay's set groups for the nested one
        MaterializeGroups(state, groups, op.vs_set, op.ps_set);
      MergeInherited(state, running, op.vs_set, kVertexShaderF);
      MergeInherited(state, running, op.ps_set, kPixelShaderF);
      Replay(state, buffers, op.command_buffer, depth + 1, predication & Predication(state));
      break;
    }
  }
  SetReplayPredicationLocked(previous_predication);
  if (replayed_draws)
    banjo::renut_gpu::CountDrawSource(1, replayed_draws);
}

// The buffer and those it runs, nested up to kMaxRunDepth, with their pending
// Begin*Vertices data copied in. Requires g_record_mutex.
void CaptureBuffers(u32 command_buffer_va, u32 depth, BufferSet &buffers) {
  if (depth >= kMaxRunDepth || buffers.contains(command_buffer_va))
    return;
  FlushPending([command_buffer_va](const PendingVertices &p) {
    return p.command_buffer == command_buffer_va;
  });
  auto it = g_buffers.find(command_buffer_va);
  if (it == g_buffers.end() || !it->second)
    return;
  buffers.emplace(command_buffer_va, it->second);
  for (const Op &op : it->second->ops) {
    if (op.kind == OpKind::Run)
      CaptureBuffers(op.command_buffer, depth + 1, buffers);
  }
}

} // namespace

bool IsRecordingCommandBuffer(u32 device_va) {
  return RecordingBuffer(device_va) != 0;
}

void BeginRecording(u32 device_va, u32 command_buffer_va) {
  std::lock_guard lock(g_record_mutex);
  FlushPending([command_buffer_va](const PendingVertices &p) {
    return p.command_buffer == command_buffer_va;
  });
  // A queued run may still hold the old recording.
  auto &slot = g_buffers[command_buffer_va];
  BumpRecordGeneration(command_buffer_va);  // banjo-nx: NewOp's per-thread lookups (blur-nx: and its buffer's own)
  if (!slot || slot.use_count() > 1) {
    slot = NewBuffer();
  } else {
    ResetForRecording(slot->ops, kMaxKeptArena / 2);  // banjo-nx (2026-10-28)
    ResetForRecording(slot->arena, kMaxKeptArena);
    slot->ClearSortPlan();  // banjo-nx (2026-10-27)
    slot->id = g_recording_ids.fetch_add(1, std::memory_order_relaxed) + 1;  // banjo-nx (2026-10-26): a new recording
  }
  // BeginCommandBuffer marks the state the buffer does not inherit dirty.
  // (The shadow stays: a buffer's first op keeps every chunk anyway.)
  DeviceRecording &set = g_recording_devices[device_va];
  set.vs = 0;
  set.ps = 0;
  std::memset(slot->set_words, 0, sizeof(slot->set_words));  // halo3-nx (2026-11-03)
  slot->inherit_runs.clear();
  slot->inherit_built = false;
  slot->inherit_chunk_ready = false;  // blur-nx (2026-11-18, #3)
  if (const auto *device = GuestPtr<const d3d::Device>(device_va)) {
    set.vs = u64(device->m_Pending[0]);
    set.ps = u64(device->m_Pending[1]);
    std::memcpy(set.begin_image, device, sizeof(set.begin_image));
  }
}

// banjo-nx (2026-10-27, banjo_renut_shadow_sort): whether a recorded draw writes depth only, so that the draws of a run
// of them leave the same depth target in any order, from the device as the op records it: a depth target; stencil
// off; no colour target whose channels are written; depth written with a less / less-equal (each sample keeps the
// smallest) or greater / greater-equal test (the largest), or not written at all; no Draw*UP data, tessellation or
// point sprites. Its pixel shader may discard (a discarded fragment writes nothing, the others still keep the
// nearest); exports, skinning once and the fur family are the replay's test (SortableShadersLocked).
// banjo-nx (2026-10-28, the console's OOM): see command_buffers.h.
void ReleaseSpareBuffersNow() {
  std::vector<std::unique_ptr<HostCommandBuffer>> spare;
  {
    std::lock_guard lock(g_free_mutex);
    spare.swap(g_free_buffers);
    g_free_bytes = 0;
  }
}

// banjo-nx (2026-10-28, the heap): see command_buffers.h.
void ReleaseSpareBuffersWhenHeapShort() {
#if defined(__SWITCH__)
  static int64_t next_ms = 0;
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  if (now < next_ms)
    return;
  next_ms = now + 5000;
  const char *top = static_cast<const char *>(sbrk(0));
  const uint64_t room = (top && fake_heap_end > top) ? uint64_t(fake_heap_end - top) : 0;
  if (room >= (uint64_t(256) << 20))
    return;
  std::vector<std::unique_ptr<HostCommandBuffer>> spare;
  {
    std::lock_guard lock(g_free_mutex);
    spare.swap(g_free_buffers);
    g_free_bytes = 0;
  }
  // (freed here, outside the lock)
#endif
}

namespace {
// banjo-nx (2026-10-27, banjo_renut_shadow_sort): each shader object's content (a 31-bit digest of its hash) and whether
// its draws may be reordered (ShaderSortableByHash), published where the object is made (hooks/shader.cpp, on the
// game's threads) and read at record time: N&B makes a shader object per mesh, so the addresses differ where the
// shader is the same. Open addressing by address, entries address << 32 | digest << 1 | sortable; a miss or a full
// probe leaves a draw unsorted (the key is only the order: any key is exact).
constexpr u32 kShaderKeyBits = 14, kShaderKeyProbes = 32;
std::atomic<uint64_t> g_shader_keys[1u << kShaderKeyBits];
inline u32 ShaderKeyIndex(u32 va) {
  return (va * 0x9E3779B1u) >> (32 - kShaderKeyBits);
}
// The shader's word (digest << 1 | sortable), 0 when unknown.
u32 ShaderSortWord(u32 va) {
  if (!va)
    return 0;
  for (u32 p = 0, i = ShaderKeyIndex(va); p < kShaderKeyProbes; ++p, i = (i + 1) & ((1u << kShaderKeyBits) - 1)) {
    const uint64_t e = g_shader_keys[i].load(std::memory_order_acquire);
    if (!e)
      return 0;
    if (u32(e >> 32) == va)
      return u32(e);
  }
  return 0;
}
}  // namespace

void NoteShaderSortKey(u32 va, uint64_t hash, bool sortable) {
  if (!va)
    return;
  const u32 digest = (u32(hash) ^ u32(hash >> 32)) & 0x7FFFFFFFu;
  const uint64_t word = uint64_t(va) << 32 | uint64_t(digest) << 1 | uint64_t(sortable ? 1 : 0);
  for (u32 p = 0, i = ShaderKeyIndex(va); p < kShaderKeyProbes; ++p, i = (i + 1) & ((1u << kShaderKeyBits) - 1)) {
    uint64_t e = g_shader_keys[i].load(std::memory_order_acquire);
    if (!e && g_shader_keys[i].compare_exchange_strong(e, word, std::memory_order_acq_rel))
      return;
    if (u32(e >> 32) == va) {
      g_shader_keys[i].store(word, std::memory_order_release);
      return;
    }
  }
}

namespace {
enum SortWhy : u8 { kSortOk, kSortOff, kSortCall, kSortNoDepth, kSortStencil, kSortColor, kSortDepthFunc, kSortShader,
                    kSortWhyCount };
u8 SortFacts(const u8 *bytes, const DrawCall &call, Op &op) {
  if (!ShadowSortOn())
    return kSortOff;
  if (call.up_vertices || call.up_indices || call.tessellation || call.primitive == 1 || !call.count)
    return kSortCall;  // (primitive 1: point list)
  const auto word = [bytes](u32 offset) {
    u32 v;
    std::memcpy(&v, bytes + offset, 4);
    return __builtin_bswap32(v);
  };
  const u32 ds = word(d3d::kDeviceOffsetDepthStencil);
  if (!ds)
    return kSortNoDepth;
  const u32 dc = word(u32(offsetof(d3d::Device, m_ControlPacket) + offsetof(d3d::ControlPacket, DepthControl)));
  if (dc & 1)
    return kSortStencil;
  const u32 mask = word(u32(offsetof(d3d::Device, m_ValuesPacket) + offsetof(d3d::ValuesPacket, ColorMask)));
  u32 rt[d3d::kMaxRenderTargets];
  for (u32 i = 0; i < d3d::kMaxRenderTargets; ++i) {
    rt[i] = word(d3d::kDeviceOffsetRenderTargets + i * 4);
    if (rt[i] && ((mask >> (4 * i)) & 0xF))
      return kSortColor;
  }
  u32 family = 3;  // depth not written
  if ((dc & 2) && (dc & 4)) {
    const u32 func = (dc >> 4) & 7;  // CompareFunction: 1 less, 3 less-equal, 4 greater, 6 greater-equal
    family = func == 1 || func == 3 ? 1u : func == 4 || func == 6 ? 2u : 0u;
    if (!family)
      return kSortDepthFunc;
  }
  const u32 vs = word(d3d::kDeviceOffsetVertexShader), ps = word(d3d::kDeviceOffsetPixelShader);
  const u32 vs_word = ShaderSortWord(vs), ps_word = ps ? ShaderSortWord(ps) : 1u;
  if (!(vs_word & 1) || !(ps_word & 1))
    return kSortShader;  // unknown, or one whose draws keep their order
  op.sort_key = uint64_t(vs_word >> 1) << 32 | (ps ? ps_word >> 1 : 0u);
  op.sort_vs = vs;
  op.sort_ps = ps;
  op.sort_decl = word(d3d::kDeviceOffsetVertexDeclaration);
  // The pipeline's render state (render/draw.cpp FastRenderState's words: the blend, colour, depth, stencil, mode and
  // colour controls, the shaders' bools) and the primitive: draws of one shader pair and render state in a row share a
  // pipeline. Only the order: any digest is exact.
  {
    constexpr u32 kControl = u32(offsetof(d3d::Device, m_ControlPacket));
    constexpr u32 kValues = u32(offsetof(d3d::Device, m_ValuesPacket));
    constexpr u32 kWords[] = {kControl + u32(offsetof(d3d::ControlPacket, BlendControl0)),
                              kControl + u32(offsetof(d3d::ControlPacket, BlendControl1)),
                              kControl + u32(offsetof(d3d::ControlPacket, BlendControl2)),
                              kControl + u32(offsetof(d3d::ControlPacket, BlendControl3)),
                              kControl + u32(offsetof(d3d::ControlPacket, ColorControl)),
                              kControl + u32(offsetof(d3d::ControlPacket, ModeControl)),
                              kValues + u32(offsetof(d3d::ValuesPacket, StencilRefMask)),
                              kValues + u32(offsetof(d3d::ValuesPacket, StencilRefMaskBF))};
    u32 h = dc * 0x9E3779B1u ^ mask ^ call.primitive;
    for (const u32 at : kWords)
      h = (h ^ word(at)) * 0x85EBCA6Bu;
    for (u32 i = 0; i < 8; ++i)  // the bools (VertexShaderB, then the pixel shader's)
      h = (h ^ word(kTailBegin + i * 4)) * 0xC2B2AE35u;
    op.sort_rs = h;
    u32 t = 0;
    for (u32 r = 0; r < 4; ++r)
      t = (t ^ word(d3d::kDeviceOffsetTextures + r * 4)) * 0x9E3779B1u;
    op.sort_tex = t;
  }
  u32 h = ds * 0x9E3779B1u ^ family;
  for (u32 i = 0; i < d3d::kMaxRenderTargets; ++i)
    h = (h ^ rt[i]) * 0x85EBCA6Bu;
  op.sort_sig = h | 1u;
  return kSortOk;
}
}  // namespace

void RecordDraw(u32 device_va, const DrawCall &call, u32 up_vertex_bytes, u32 up_index_bytes) {
  HostCommandBuffer *buffer = nullptr;
  Op *op = NewOp(device_va, OpKind::Draw, &buffer);
  if (!op)
    return;
  // banjo-nx (2026-10-27, banjo_renut_shadow_sort): only with the sort on (PlanSortedRuns is its only reader; about
  // thirty device words and three hashes a recorded draw on the draw threads for nothing since the sort went off).
  if (ShadowSortOn()) {
    if (const auto *bytes = GuestPtr<const u8>(device_va))
      op->sort_why = SortFacts(bytes, call, *op);
  }
  SetOpCall(op->call, call, GuestPtr<const u8>(device_va));
  if (call.up_vertices)
    op->call.up_vertices = u32(CopyIn(*buffer, call.up_vertices, up_vertex_bytes));
  if (call.up_indices)
    op->call.up_indices = u32(CopyIn(*buffer, call.up_indices, up_index_bytes));
}

void RecordDeferredDraw(u32 device_va, const DrawCall &call, const u8 *scratch, u32 bytes) {
  HostCommandBuffer *buffer = nullptr;
  Op *op = NewOp(device_va, OpKind::Draw, &buffer);
  if (!op)
    return;
  SetOpCall(op->call, call, GuestPtr<const u8>(device_va));
  op->call.up_vertices = u32(Reserve(*buffer, bytes));  // (Begin*Vertices draws have no indices)
  std::lock_guard lock(g_record_mutex);
  g_pending_vertices.push_back(
      {device_va, RecordingBuffer(device_va), op->call.up_vertices, scratch, bytes});
  g_recording_devices[device_va].pending.fetch_add(1, std::memory_order_acq_rel);  // blur-nx (2026-11-17)
  PendingOf(device_va).fetch_add(1, std::memory_order_acq_rel);
  SyncPendingCount();
}

void RecordClear(u32 device_va, u32 flags, const ClearRect *rect, const float color[4],
                 float depth, u32 stencil) {
  HostCommandBuffer *buffer = nullptr;
  Op *op = NewOp(device_va, OpKind::Clear, &buffer);
  if (!op)
    return;
  op->flags = flags;
  op->has_rect = rect != nullptr;
  if (rect)
    op->rect = *rect;
  std::memcpy(op->color, color, sizeof(op->color));
  op->depth = depth;
  op->stencil = stencil;
}

void RecordResolve(u32 device_va, u32 flags, const ClearRect *source_rect, u32 dest_texture_va,
                   i32 dest_x, i32 dest_y, u32 dest_level, u32 dest_slice,
                   const float clear_color[4], float clear_depth, u32 clear_stencil) {
  HostCommandBuffer *buffer = nullptr;
  Op *op = NewOp(device_va, OpKind::Resolve, &buffer);
  if (!op)
    return;
  op->flags = flags;
  op->has_rect = source_rect != nullptr;
  if (source_rect)
    op->rect = *source_rect;
  op->has_color = clear_color != nullptr;
  if (clear_color)
    std::memcpy(op->color, clear_color, sizeof(op->color));
  op->depth = clear_depth;
  op->stencil = clear_stencil;
  op->dest_texture = dest_texture_va;
  if (const auto *header = GuestPtr<const d3d::BaseTexture>(dest_texture_va)) {
    op->has_dest_format = true;
    op->dest_format = header->Format;
  }
  op->dest_x = dest_x;
  op->dest_y = dest_y;
  op->dest_level = dest_level;
  op->dest_slice = dest_slice;
}

// banjo-nx (2026-10-26, banjo_renut_diag_fast_census): the 10 s line of CensusRecording.
// banjo-nx (2026-10-28, banjo_renut_diag_packet_census): the counts since the start.
std::string PacketCensusReport() {
  if (!g_packet_census || !g_packet_census->draws)
    return {};
  const PacketCensus &c = *g_packet_census;
  const auto pct = [](uint64_t a, uint64_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; };
  static constexpr const char *kNames[PacketCensus::kFieldCount] = {
      "fetch", "bools", "dest", "view packets", "misc", "29C0", "unread", "private", "decl",
      "tail", "index", "targets", "streams", "tex words", "scissor", "shaders", "predication", "vertex fetch"};
  std::string fields = "; drawn recently without the field / the field alone:";
  for (u32 fi = 0; fi < PacketCensus::kFieldCount; ++fi)
    fields += fmt::format(" {} {:.0f}/{:.0f} %", kNames[fi], pct(c.without_recent[fi], c.draws),
                          pct(c.alone_recent[fi], c.draws));
  return fmt::format("host packets census (banjo_renut_diag_packet_census): {} replayed draws, {} distinct states "
                     "({:.1f} % new); a state drawn this frame or the one before {:.1f} %, in an 8,192-entry table {:.1f} "
                     "%; the {} draws that re-derive something today ({:.1f} %): {:.1f} % drawn recently, {:.1f} % in the "
                     "table; shaders or declaration changed {} ({:.1f} % in the table), textures {} ({:.1f} %), "
                     "geometry {} ({:.1f} %); packet keys: {} keyed, {} not; a recorded key not the state's own {} (must "
                     "be 0), draws whose key another whole state had {}",
                     c.draws, c.seen.size(), pct(c.keys_new, c.draws), pct(c.seen_recent, c.draws), pct(c.hit, c.draws),
                     c.derive, pct(c.derive, c.draws), pct(c.derive_seen_recent, c.derive), pct(c.derive_hit, c.derive),
                     c.derive_shaders, pct(c.derive_shaders_hit, c.derive_shaders), c.derive_textures,
                     pct(c.derive_textures_hit, c.derive_textures), c.derive_geometry,
                     pct(c.derive_geometry_hit, c.derive_geometry), c.keyed, c.unkeyed, c.key_two_states,
                     c.state_two_keys) +
         fields;
}

std::string ReplayRepeatCensusReport() {
  RepeatCensus &c = g_repeat_census;
  if (!c.recordings)
    return {};
  const double d = c.draws ? double(c.draws) : 1.0;
  std::string out = fmt::format("recordings against the frame before's (banjo_renut_diag_fast_census): {} replayed "
                                "({} draws); the same ops and device chunks {} ({:.1f} % of the draws), and the same "
                                "float constants too {} ({:.1f} % of the draws)",
                                c.recordings, c.draws, c.same_structure, 100.0 * double(c.same_structure_draws) / d,
                                c.same_all, 100.0 * double(c.same_all_draws) / d);
  // banjo-nx (2026-10-26, setter rewrite): by op, the frame before's at the same place.
  if (c.compared_draws) {
    const double cd = double(c.compared_draws);
    static constexpr const char *kNames[18] = {"fetch", "bools", "dest", "view packets", "misc", "29C0", "unread",
                                               "private", "decl", "tail", "index", "targets", "streams", "tex words",
                                               "scissor", "shaders", "predication", "vertex fetch"};
    out += fmt::format("; by op: {} draws compared, the same op as the frame before's at its place {:.1f} %, every op "
                       "up to it the same {:.1f} %; the others' fields that differ:",
                       c.compared_draws, 100.0 * double(c.op_same_draws) / cd, 100.0 * double(c.prefix_same_draws) / cd);
    const double other = std::max(1.0, cd - double(c.op_same_draws));
    for (u32 f = 0; f < 18; ++f)
      if (c.differ_fields[f])
        out += fmt::format(" {} {:.0f} %", kNames[f], 100.0 * double(c.differ_fields[f]) / other);
  }
  c.recordings = c.draws = c.same_structure = c.same_structure_draws = c.same_all = c.same_all_draws = 0;
  c.op_same_draws = c.prefix_same_draws = c.compared_draws = 0;
  std::fill(std::begin(c.differ_fields), std::end(c.differ_fields), uint64_t(0));
  return out;
}

void RecordRun(u32 device_va, u32 command_buffer_va) {
  HostCommandBuffer *buffer = nullptr;
  if (Op *op = NewOp(device_va, OpKind::Run, &buffer))
    op->command_buffer = command_buffer_va;
}

void FlushRecordedVertices(u32 device_va) {
  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_callbacks_fast): every hook calls this first; nothing pending (always so without
  // command buffers, SA2's case) needs no lock. g_pending_count is kept under g_record_mutex (SyncPendingCount).
  // blur-nx (2026-11-17, blur_renut_flush_fast_local): this device's count (FlushDevicePending takes nothing else).
  static const bool local = REXCVAR_GET(blur_renut_flush_fast_local);
  if (HookFastPathsOn() && (local ? PendingOf(device_va).load(std::memory_order_acquire) == 0
                                  : g_pending_count.load(std::memory_order_acquire) == 0))
    return;
  std::lock_guard lock(g_record_mutex);
  FlushDevicePending(device_va);
}

namespace {
// banjo-nx (2026-10-27, banjo_renut_shadow_sort): the plan's census (the 10 s line), from the threads that queue runs.
constexpr u32 kMinSortRun = 3;
constexpr u32 kSortSlots = 192;  // 64 plain chunk slots (53 used), 64 + 64 constant groups
std::atomic<uint64_t> g_sort_plans{0}, g_sort_runs{0}, g_sort_run_draws{0}, g_sort_sorted_runs{0}, g_sort_sorted_draws{0};
std::atomic<uint64_t> g_sort_changes_before{0}, g_sort_changes_after{0}, g_sort_pairs{0}, g_sort_draws{0};
std::atomic<uint64_t> g_sort_why[kSortWhyCount], g_sort_lengths[6], g_sort_end[6];

// An op's chunk and constant group copies in the arena (CopyState's order: the chunks by index, then the vertex
// shader's groups and the pixel shader's, each by group).
void ApplyVersions(const Op &op, u32 *ver) {
  if (op.state == kNone)
    return;
  u32 at = u32(op.state);
  for (uint64_t c = op.plain_dirty; c; c &= c - 1) {
    const u32 i = u32(std::countr_zero(c));
    ver[i] = at;
    at += ChunkSize(i);
  }
  for (uint64_t m = op.vs_dirty; m;) {  // bit 63 - group: the highest bit is the lowest group, copied first
    const u32 g = u32(std::countl_zero(m));
    ver[64 + g] = at;
    at += kConstantGroupBytes;
    m &= ~(uint64_t(1) << (63 - g));
  }
  for (uint64_t m = op.ps_dirty; m;) {
    const u32 g = u32(std::countl_zero(m));
    ver[128 + g] = at;
    at += kConstantGroupBytes;
    m &= ~(uint64_t(1) << (63 - g));
  }
}

// The sorted runs of a captured buffer (once; the render thread reads them at sort_state 2). A run: from op 1 on,
// consecutive draws with one signature (SortFacts), at least kMinSortRun, not in order already. Each draw's state is
// the versions (arena copies) of the slots the run's ops write; the sorted order's entries carry the slots whose version
// differs from the device the draw before left, the groups whose source changes (op set / the running device), and a
// last entry back to the run's last op as recorded.
void PlanSortedRuns(HostCommandBuffer &b) {
  u32 expected = 0;
  if (!ShadowSortOn() || !b.sort_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
    return;
  const Op *const ops = b.ops.data();
  const u32 n = u32(b.ops.size());
  uint64_t why[kSortWhyCount] = {}, lengths[6] = {}, ends[6] = {}, draws = 0;
  struct Cand {
    u32 begin, end;
  };
  std::vector<Cand> cands;
  for (u32 i = 0; i < n;) {
    const Op &o = ops[i];
    if (o.kind == OpKind::Draw) {
      ++draws;
      ++why[o.sort_sig ? kSortOk : o.sort_why && o.sort_why < kSortWhyCount ? o.sort_why : kSortCall];  // (Begin*Vertices: no facts)
    }
    if (o.kind != OpKind::Draw || !o.sort_sig || o.state == kNone || i == 0) {
      ++i;
      continue;
    }
    u32 j = i + 1;
    while (j < n && ops[j].kind == OpKind::Draw && ops[j].sort_sig == o.sort_sig && ops[j].state != kNone) {
      ++draws;
      ++why[kSortOk];
      ++j;
    }
    const u32 len = j - i;
    ++lengths[len <= 2 ? 0 : len <= 4 ? 1 : len <= 8 ? 2 : len <= 16 ? 3 : len <= 64 ? 4 : 5];
    // What ended it: the buffer's end, a run op, a clear or resolve, a draw of other targets, an unsortable draw.
    ++ends[j >= n ? 0 : ops[j].kind == OpKind::Run ? 1 : ops[j].kind != OpKind::Draw ? 2 : ops[j].sort_sig ? 3 : 4];
    if (len >= kMinSortRun)
      cands.push_back({i, j});
    i = j;
  }
  g_sort_plans.fetch_add(1, std::memory_order_relaxed);
  g_sort_draws.fetch_add(draws, std::memory_order_relaxed);
  for (u32 k = 0; k < kSortWhyCount; ++k)
    if (why[k])
      g_sort_why[k].fetch_add(why[k], std::memory_order_relaxed);
  for (u32 k = 0; k < 6; ++k) {
    if (lengths[k])
      g_sort_lengths[k].fetch_add(lengths[k], std::memory_order_relaxed);
    if (ends[k])
      g_sort_end[k].fetch_add(ends[k], std::memory_order_relaxed);
  }
  uint64_t runs = 0, run_draws = 0, sorted_runs = 0, sorted_draws = 0, before = 0, after = 0;
  u32 ver[kSortSlots] = {};
  u32 applied = 0;  // ops [0, applied) are in 'ver'
  std::vector<u32> verw, order;
  std::vector<u8> slots;
  for (const Cand &c : cands) {
    const u32 len = c.end - c.begin;
    ++runs;
    run_draws += len;
    order.resize(len);
    for (u32 k = 0; k < len; ++k)
      order[k] = k;
    const auto less = [&](u32 x, u32 y) {
      const Op &a = ops[c.begin + x], &bo = ops[c.begin + y];
      if (a.sort_key != bo.sort_key)
        return a.sort_key < bo.sort_key;
      if (a.sort_rs != bo.sort_rs)
        return a.sort_rs < bo.sort_rs;
      return a.sort_tex != bo.sort_tex ? a.sort_tex < bo.sort_tex : a.sort_decl < bo.sort_decl;
    };
    std::stable_sort(order.begin(), order.end(), less);
    u32 changes_before = 0, changes_after = 0;
    bool identity = true;
    for (u32 k = 0; k < len; ++k) {
      identity = identity && order[k] == k;
      if (k) {
        changes_before += ops[c.begin + k].sort_key != ops[c.begin + k - 1].sort_key;
        changes_after += ops[c.begin + order[k]].sort_key != ops[c.begin + order[k - 1]].sort_key;
      }
    }
    before += changes_before;
    after += identity ? changes_before : changes_after;
    if (identity)
      continue;
    // The slots the run's ops write, and the versions of each op (as recorded) of those.
    uint64_t plain = 0, vsw = 0, psw = 0;
    for (u32 k = c.begin; k < c.end; ++k) {
      plain |= ops[k].plain_dirty;
      vsw |= ops[k].vs_dirty;
      psw |= ops[k].ps_dirty;
    }
    slots.clear();
    for (uint64_t m = plain; m; m &= m - 1)
      slots.push_back(u8(std::countr_zero(m)));
    for (u32 g = 0; g < 64; ++g)
      if ((vsw >> (63 - g)) & 1)
        slots.push_back(u8(64 + g));
    for (u32 g = 0; g < 64; ++g)
      if ((psw >> (63 - g)) & 1)
        slots.push_back(u8(128 + g));
    const u32 w = u32(slots.size());
    for (; applied < c.begin; ++applied)
      ApplyVersions(ops[applied], ver);
    std::vector<u32> cur(w);
    for (u32 k = 0; k < w; ++k)
      cur[k] = ver[slots[k]];
    verw.resize(size_t(len) * w);
    for (u32 j = 0; j < len; ++j) {
      ApplyVersions(ops[c.begin + j], ver);
      for (u32 k = 0; k < w; ++k)
        verw[size_t(j) * w + k] = ver[slots[k]];
    }
    applied = c.end;
    SortRun run;
    run.begin = c.begin;
    run.end = c.end;
    run.first_entry = u32(b.sort_entries.size());
    uint64_t prev_vs = ops[c.begin - 1].vs_set, prev_ps = ops[c.begin - 1].ps_set;
    const auto emit = [&](u32 op_index, const u32 *target, uint64_t vs_set, uint64_t ps_set) {
      SortEntry e;
      e.op = op_index;
      e.first_pair = u32(b.sort_pairs.size());
      for (u32 k = 0; k < w; ++k) {
        if (target[k] == cur[k])
          continue;
        const u32 slot = slots[k];
        // A chunk holding several fields (the targets share one with the index buffer and the streams): the fields of
        // the words that differ between the copy the device holds and the new one, as NewOp finds them; another chunk
        // as a whole. (Whole-chunk fields told the fast path its targets had changed at nearly every sorted draw.)
        if (slot < 64 && ((kPlainFields.mixed >> slot) & 1)) {
          const u8 *was = b.arena.data() + cur[k], *now = b.arena.data() + target[k];
          for (u32 words = ChunkWordMask(now, was, ChunkSize(slot)); words; words &= words - 1)
            e.fields |= kPlainFields.word[slot][std::countr_zero(words)];
        } else if (slot < 64) {
          e.fields |= kPlainFields.chunk[slot];
        }
        cur[k] = target[k];
        b.sort_pairs.push_back({slot, target[k]});
        if (slot < 64)
          e.plain |= uint64_t(1) << slot;
        else if (slot < 128)
          e.vs_dirty |= uint64_t(1) << (63 - (slot - 64));
        else
          e.ps_dirty |= uint64_t(1) << (63 - (slot - 128));
      }
      e.pair_count = u32(b.sort_pairs.size()) - e.first_pair;
      // A group read from the op's state by one draw and from the running device by the other changes too.
      e.vs_dirty |= prev_vs ^ vs_set;
      e.ps_dirty |= prev_ps ^ ps_set;
      prev_vs = vs_set;
      prev_ps = ps_set;
      b.sort_entries.push_back(e);
    };
    for (u32 k = 0; k < len; ++k) {
      const Op &o = ops[c.begin + order[k]];
      emit(c.begin + order[k], &verw[size_t(order[k]) * w], o.vs_set, o.ps_set);
    }
    emit(kSortRestore, &verw[size_t(len - 1) * w], ops[c.end - 1].vs_set, ops[c.end - 1].ps_set);
    run.entry_count = u32(b.sort_entries.size()) - run.first_entry;
    b.sort_runs.push_back(run);
    ++sorted_runs;
    sorted_draws += len;
  }
  g_sort_runs.fetch_add(runs, std::memory_order_relaxed);
  g_sort_run_draws.fetch_add(run_draws, std::memory_order_relaxed);
  g_sort_sorted_runs.fetch_add(sorted_runs, std::memory_order_relaxed);
  g_sort_sorted_draws.fetch_add(sorted_draws, std::memory_order_relaxed);
  g_sort_changes_before.fetch_add(before, std::memory_order_relaxed);
  g_sort_changes_after.fetch_add(after, std::memory_order_relaxed);
  g_sort_pairs.fetch_add(b.sort_pairs.size(), std::memory_order_relaxed);
  b.sort_state.store(2, std::memory_order_release);
}
}  // namespace

namespace {
struct Run {
  std::vector<u8> running;  // the running device, whose constants the buffer inherits
  BufferSet buffers;
};

// blur-nx (2026-11-17, blur_renut_run_pool): the runs given back (the render thread's replay done, or a held tile run
// dropped). A run's buffers are let go when it is given back, as when it was deleted; its device copy keeps its
// storage, so the next run's copy is not an allocation.
constexpr size_t kRunPoolMax = 64;
std::mutex g_run_pool_mutex;
std::vector<Run *> g_run_pool;

void GiveRun(Run *run) {
  run->buffers.clear();
  {
    std::lock_guard lock(g_run_pool_mutex);
    if (g_run_pool.size() < kRunPoolMax) {
      g_run_pool.push_back(run);
      return;
    }
  }
  delete run;
}

// blur-nx (2026-11-17, the Renut audit's #25 and #26, blur_renut_replay_pool): a run's shared_ptr control block from a
// free list (shared_ptr's allocator form: one block size, that of the control block of a Run with GiveRun), and the
// replay's closure a pooled job, so the render thread's std::function holds one pointer (stored in place, no heap) and
// not a shared_ptr and two words (24 bytes, a heap allocation each replay).
bool ReplayPoolOn() {
  static const bool on = REXCVAR_GET(blur_renut_replay_pool);
  return on;
}
template <typename T>
struct RunBlockAlloc {
  using value_type = T;
  RunBlockAlloc() = default;
  template <typename U>
  RunBlockAlloc(const RunBlockAlloc<U> &) {}
  static std::mutex &Mutex() {
    static std::mutex m;
    return m;
  }
  static std::vector<void *> &Free() {
    static std::vector<void *> v;
    return v;
  }
  T *allocate(size_t n) {
    if (n == 1) {
      std::lock_guard lock(Mutex());
      if (!Free().empty()) {
        void *p = Free().back();
        Free().pop_back();
        return static_cast<T *>(p);
      }
    }
    return static_cast<T *>(::operator new(n * sizeof(T)));
  }
  void deallocate(T *p, size_t n) {
    if (n == 1) {
      std::lock_guard lock(Mutex());
      if (Free().size() < 256) {
        Free().push_back(p);
        return;
      }
    }
    ::operator delete(p);
  }
  template <typename U>
  bool operator==(const RunBlockAlloc<U> &) const { return true; }
  template <typename U>
  bool operator!=(const RunBlockAlloc<U> &) const { return false; }
};

struct ReplayJob {
  std::shared_ptr<Run> run;
  u32 command_buffer_va = 0;
  u32 predication = 0;
};
std::mutex g_replay_jobs_mutex;
std::vector<ReplayJob *> g_replay_jobs;
ReplayJob *TakeReplayJob() {
  {
    std::lock_guard lock(g_replay_jobs_mutex);
    if (!g_replay_jobs.empty()) {
      ReplayJob *job = g_replay_jobs.back();
      g_replay_jobs.pop_back();
      return job;
    }
  }
  return new ReplayJob;
}
void GiveReplayJob(ReplayJob *job) {
  job->run.reset();  // the run back to its pool now, as the closure's copy went when it was destroyed
  {
    std::lock_guard lock(g_replay_jobs_mutex);
    if (g_replay_jobs.size() < 256) {
      g_replay_jobs.push_back(job);
      return;
    }
  }
  delete job;
}

std::shared_ptr<Run> NewRun() {
  static const bool pool = REXCVAR_GET(blur_renut_run_pool);
  if (!pool)
    return std::make_shared<Run>();
  Run *run = nullptr;
  {
    std::lock_guard lock(g_run_pool_mutex);
    if (!g_run_pool.empty()) {
      run = g_run_pool.back();
      g_run_pool.pop_back();
    }
  }
  if (ReplayPoolOn())  // blur-nx (2026-11-17, audit #26)
    return std::shared_ptr<Run>(run ? run : new Run, GiveRun, RunBlockAlloc<Run>{});
  return std::shared_ptr<Run>(run ? run : new Run, GiveRun);
}

void QueueReplay(std::shared_ptr<Run> run, u32 command_buffer_va, u32 predication) {
  if (ReplayPoolOn()) {  // blur-nx (2026-11-17, audit #25)
    ReplayJob *job = TakeReplayJob();
    job->run = std::move(run);
    job->command_buffer_va = command_buffer_va;
    job->predication = predication;
    render_thread::Queue([job] {
      Replay(job->run->running.data(), job->run->buffers, job->command_buffer_va, 0, job->predication);
      GiveReplayJob(job);
    });
    return;
  }
  render_thread::Queue([run, command_buffer_va, predication] {
    Replay(run->running.data(), run->buffers, command_buffer_va, 0, predication);
  });
}

// vivapinata-nx (2026-11-11, vivapinata_tiles_once / A/B 86): a run under one tile's predication is held here until
// the next thing the game queues. The same buffer for another tile joins it (one replay with both tiles' masks); anything
// else, or any queue or wait (render_thread::SetBeforePushHook), queues it as it was first.
struct HeldTileRun {
  std::shared_ptr<Run> run;
  u32 command_buffer = 0;
  u32 predication = 0;  // the tiles' masks so far
  u32 device_va = 0;
};
std::mutex g_tiles_once_mutex;
HeldTileRun g_held;  // run null: none
std::atomic<uint64_t> g_tiles_once_merged{0}, g_tiles_once_single{0};
std::atomic<uint32_t> g_tiles_once_why[6];  // per-tile runs by reason, below
constexpr const char *kTilesOnceWhy[6] = {"another buffer", "a clear or resolve in it", "the running device differs",
                                          "another device", "a tile already held", "held until something else came"};

// The one tile a predication mask selects among 'tiles', or -1 (none or several).
int OneTile(u32 mask, u32 tiles) {
  int tile = -1;
  for (u32 i = 0; i < tiles && i < 16; ++i) {
    if (!(mask & (3u << (2 * i))))
      continue;
    if (tile >= 0)
      return -1;
    tile = int(i);
  }
  return tile;
}

bool HasClearOrResolve(const BufferSet &buffers) {
  for (const auto &[va, buffer] : buffers) {
    if (!buffer)
      continue;
    for (const Op &op : buffer->ops) {
      if (op.kind == OpKind::Clear || op.kind == OpKind::Resolve)
        return true;
    }
  }
  return false;
}

bool SameRecordings(const BufferSet &a, const BufferSet &b) {
  if (a.size() != b.size())
    return false;
  for (const auto &[va, buffer] : a) {
    auto it = b.find(va);
    if (it == b.end() || it->second != buffer)
      return false;
  }
  return true;
}

// The two runs' devices, the same but for the predication words (the tile's run and its twin), else the first
// offset that differs.
bool SameButPredication(const std::vector<u8> &a, const std::vector<u8> &b, u32 &differs) {
  if (a.size() != b.size()) {
    differs = 0;
    return false;
  }
  // The device's header (+0x00 m_Pending's dirty masks, +0x28 m_pRing and its limits, +0x34 m_ReferenceCount) is the
  // command writer's bookkeeping, which moves between the two runs, not state a draw reads (d3d.h's Device).
  u32 begin = 0x38;
  auto same = [&](u32 end) {
    if (end > begin && std::memcmp(a.data() + begin, b.data() + begin, end - begin) != 0) {
      for (u32 i = begin; i < end; ++i) {
        if (a[i] != b[i]) {
          differs = i;
          break;
        }
      }
      return false;
    }
    return true;
  };
  // Ascending: TilePredication and RunPredication (SetCommandBufferPredication's), then the device's predication and
  // run predication state.
  // banjo-nx (2026-11-19, its rebase on the base): the XDK's own offsets (d3d_native.h). Port 87 wrote
  // vivapinata-nx's 0x2A30 / 0x2A34, which are other fields at N&B's and Katamari's XDKs (TilePredication 0x2AB4).
  constexpr u32 kSkipped[4] = {native::off::TilePredication, native::off::RunPredication, d3d::kDeviceOffsetPredication,
                               d3d::kDeviceOffsetPredication + 4};
  static_assert(native::off::TilePredication < native::off::RunPredication &&
                    native::off::RunPredication < d3d::kDeviceOffsetPredication,
                "kSkipped ascending");
  for (const u32 skip : kSkipped) {
    if (skip + 4 > a.size())
      break;
    if (!same(skip))
      return false;
    begin = skip + 4;
  }
  return same(u32(a.size()));
}

void FlushHeldTileRun() {
  HeldTileRun held;
  {
    std::lock_guard lock(g_tiles_once_mutex);
    if (!g_held.run)
      return;
    held = std::move(g_held);
    g_held = {};
  }
  g_tiles_once_single.fetch_add(1, std::memory_order_relaxed);
  g_tiles_once_why[5].fetch_add(1, std::memory_order_relaxed);
  QueueReplay(held.run, held.command_buffer, held.predication);
}

void ReportTilesOnce() {
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(10))
    return;
  last = now;
  std::string why;
  for (int i = 0; i < 5; ++i) {
    if (const uint32_t n = g_tiles_once_why[i].exchange(0, std::memory_order_relaxed))
      why += fmt::format("{}{} {}", why.empty() ? "" : ", ", n, kTilesOnceWhy[i]);
  }
  g_tiles_once_why[5].store(0, std::memory_order_relaxed);
  RGPU_INFO("[renut] tiles once (vivapinata_tiles_once, A/B 86): {} tile runs replayed once for every tile, {} per tile "
            "in the last 10 s{}{}",
            g_tiles_once_merged.exchange(0, std::memory_order_relaxed),
            g_tiles_once_single.exchange(0, std::memory_order_relaxed), why.empty() ? "" : ": ", why);
}

// True when the run was taken (held or merged): nothing more to queue.
bool TilesOnce(u32 device_va, const u8 *device, u32 command_buffer_va, const std::shared_ptr<Run> &run) {
  static const bool on = [] {
    const bool v = REXCVAR_GET(vivapinata_tiles_once);
    if (v)
      render_thread::SetBeforePushHook(&FlushHeldTileRun);
    return v;
  }();
  if (!on)
    return false;
  ReportTilesOnce();
  if (banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) == 86)
    return false;
  // Tiling (BeginTiling sets Flags 0x20 until EndTiling) with two tiles or more.
  // banjo-nx (2026-11-19, its rebase on the base): native::off's TileCount and Flags (the byte BeginTiling sets 0x20
  // in, hooks/draw.cpp), not port 87's vivapinata-nx offsets 0x30B8 / 0x2A38, other fields at this XDK (its Flags is
  // TilePredication + 8, as here).
  const u32 tiles = *reinterpret_cast<const be_u32 *>(device + native::off::TileCount);
  if (!(device[native::off::Flags] & 0x20) || tiles < 2 || tiles > 16)
    return false;
  const u32 predication = Predication(run->running.data());
  const int tile = OneTile(predication, tiles);
  if (tile < 0)
    return false;
  std::unique_lock lock(g_tiles_once_mutex);
  if (g_held.run) {
    int why = -1;
    u32 differs = 0;
    if (g_held.device_va != device_va)
      why = 3;
    else if (g_held.command_buffer != command_buffer_va || !SameRecordings(g_held.run->buffers, run->buffers))
      why = 0;
    else if (g_held.predication & (3u << (2 * tile)))
      why = 4;
    else if (!SameButPredication(g_held.run->running, run->running, differs))
      why = 2;
    if (why < 0) {
      // The twin: one run for both. The running device's own predication word is both tiles' too (draws that set none
      // take it); the replay's mask is the union.
      g_held.predication |= predication;
      const u32 word_a = *reinterpret_cast<const be_u32 *>(g_held.run->running.data() + d3d::kDeviceOffsetPredication);
      const u32 word_b = *reinterpret_cast<const be_u32 *>(run->running.data() + d3d::kDeviceOffsetPredication);
      *reinterpret_cast<be_u32 *>(g_held.run->running.data() + d3d::kDeviceOffsetPredication) =
          (word_a && word_b) ? (word_a | word_b) : 0u;
      g_tiles_once_merged.fetch_add(1, std::memory_order_relaxed);
      // Every tile in: queue it now.
      bool all = true;
      for (u32 i = 0; i < tiles; ++i)
        all = all && (g_held.predication & (3u << (2 * i)));
      if (all) {
        HeldTileRun held = std::move(g_held);
        g_held = {};
        lock.unlock();
        QueueReplay(held.run, held.command_buffer, held.predication);
      }
      return true;
    }
    if (why == 2) {
      static uint32_t logged = 0;
      if (logged++ < 8)
        RGPU_INFO("[renut] tiles once: buffer {:08X}'s runs for two tiles differ at device offset 0x{:X}: per tile",
                  command_buffer_va, differs);
    }
    g_tiles_once_why[why].fetch_add(1, std::memory_order_relaxed);
    // The held one goes as it was; this one is held in its place (it may have a twin of its own).
    HeldTileRun held = std::move(g_held);
    g_held = {};
    lock.unlock();
    g_tiles_once_single.fetch_add(1, std::memory_order_relaxed);
    QueueReplay(held.run, held.command_buffer, held.predication);
    lock.lock();
  }
  if (HasClearOrResolve(run->buffers)) {
    g_tiles_once_why[1].fetch_add(1, std::memory_order_relaxed);
    g_tiles_once_single.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  g_held.run = run;
  g_held.command_buffer = command_buffer_va;
  g_held.predication = predication;
  g_held.device_va = device_va;
  return true;
}
}  // namespace

void QueueRunCommandBuffer(u32 device_va, u32 command_buffer_va) {
  FlushDeferredVertices(device_va);
  const u8 *device = GuestPtr<const u8>(device_va);
  if (!device)
    return;
  auto run = NewRun();  // blur-nx (2026-11-17, blur_renut_run_pool)
  run->running.assign(device, device + d3d::kDeviceSnapshotSize);
  {
    // blur-nx (2026-11-18, the Renut host batch's census, #28): the time to take the record lock here (the Game Master).
    const bool hb_census = ::renut::gpu::host_batch::CensusOn();
    const uint64_t t0 = hb_census ? ::renut::gpu::host_batch::Ticks() : 0;
    std::lock_guard lock(g_record_mutex);
    if (hb_census) {
      const uint64_t waited = ::renut::gpu::host_batch::Ticks() - t0;
      ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kRecordLockTicks, waited);
      if (waited > 19)  // over about a microsecond (the 19.2 MHz counter)
        ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kRecordLockWaits);
    }
    CaptureBuffers(command_buffer_va, 0, run->buffers);
  }
  // banjo-nx (2026-10-27, banjo_renut_shadow_sort): each captured recording's sorted runs, here and not on the render
  // thread (once a recording: a buffer run again keeps its plan).
  if (ShadowSortOn()) {
    for (const auto &[va, recording] : run->buffers)
      if (recording)
        PlanSortedRuns(*recording);
  }
  if (UnleashedCensusOn()) {  // banjo-nx (2026-10-26): each recording of the run, with its draws
    const uint64_t swap = rex::latency::SwapCount();
    for (const auto &[va, recording] : run->buffers) {
      if (!recording)
        continue;
      u32 draws = 0;
      for (const Op &op : recording->ops)
        draws += op.kind == OpKind::Draw;
      NoteCommandBufferRun(reinterpret_cast<const void *>(uintptr_t(recording->id)), draws, swap);
    }
  }
  // vivapinata-nx (2026-11-11, vivapinata_tiles_once): a tile's run may wait for its twin (TilesOnce).
  if (TilesOnce(device_va, device, command_buffer_va, run))
    return;
  QueueReplay(run, command_buffer_va, Predication(run->running.data()));
}

void ReleaseCommandBuffer(u32 command_buffer_va) {
  std::lock_guard lock(g_record_mutex);
  if (g_buffers.erase(command_buffer_va)) {
    BumpRecordGeneration(command_buffer_va);  // banjo-nx: NewOp's per-thread lookups (blur-nx: and its buffer's own)
    for (size_t i = 0; i < g_pending_vertices.size();) {
      if (g_pending_vertices[i].command_buffer == command_buffer_va) {
        g_recording_devices[g_pending_vertices[i].device].pending.fetch_sub(1, std::memory_order_acq_rel);  // blur-nx
        PendingOf(g_pending_vertices[i].device).fetch_sub(1, std::memory_order_acq_rel);
        g_pending_vertices[i] = g_pending_vertices.back();
        g_pending_vertices.pop_back();
      } else {
        ++i;
      }
    }
    SyncPendingCount();
  }
}

// blur-nx (2026-11-17): NewOp's memo census and the record lock's slow paths since the last report. The memos of the
// devices with a touch slot are summed (each written by the one thread recording its device; read here without a lock,
// a census); those of the threads without one are not reachable and are left out.
std::string RecordLockReport() {
  static uint64_t last[4] = {};
  uint64_t now[4] = {};
  for (const SlotRecordCache &s : g_record_cache_slots) {
    now[0] += s.c.hits;
    now[1] += s.c.miss_other;
    now[2] += s.c.miss_generation;
    now[3] += s.c.miss_pending;
  }
  uint64_t d[4];
  for (int i = 0; i < 4; ++i) {
    d[i] = now[i] - last[i];
    last[i] = now[i];
  }
  const uint64_t calls = d[0] + d[1] + d[2] + d[3];
  const uint64_t slow = g_record_mutex.slow.exchange(0, std::memory_order_relaxed);
  const uint64_t yields = g_record_mutex.yields.exchange(0, std::memory_order_relaxed);
  const uint64_t naps = g_record_mutex.naps.exchange(0, std::memory_order_relaxed);
  if (!calls && !slow)
    return {};
  return fmt::format("record lock: NewOp {} ops, {} answered by the memo ({:.1f} %), misses: {} another device or "
                     "buffer, {} the buffer begun or changed, {} vertices pending; spin lock (blur_renut_record_spin "
                     "{}): {} slow paths, {} yields, {} naps of 20 us",
                     calls, d[0], calls ? 100.0 * double(d[0]) / double(calls) : 0.0, d[1], d[2], d[3],
                     REXCVAR_GET(blur_renut_record_spin) ? "on" : "off", slow, yields, naps);
}

// banjo-nx (2026-10-26, banjo_renut_diag_fast_census): the setters' and the inline writes' shares of the changed state.
std::string StateSourceReport() {
  const uint64_t setter = g_census_setter_changed.exchange(0, std::memory_order_relaxed);
  const uint64_t inl = g_census_inline_changed.exchange(0, std::memory_order_relaxed);
  const uint64_t touch_marks = g_census_touch_marks.exchange(0, std::memory_order_relaxed);
  const uint64_t tag_only = g_census_tag_only_marks.exchange(0, std::memory_order_relaxed);
  const uint64_t census_ops = g_census_ops.exchange(0, std::memory_order_relaxed);
  if (!setter && !inl)
    return {};
  static constexpr const char *kNames[18] = {"fetch", "bools", "dest", "view packets", "misc", "29C0", "unread",
                                             "private", "decl", "tail", "index", "targets", "streams", "tex words",
                                             "scissor", "shaders", "predication", "vertex fetch"};
  std::string out = fmt::format("state by source (banjo_renut_diag_fast_census): {} changed chunks marked by the hooked "
                                "setters, {} only by an inline write's pending tag ({:.1f} % inline); setters by field:",
                                setter, inl, 100.0 * double(inl) / double(setter + inl));
  if (census_ops)
    out += fmt::format(" marks an op: {:.1f} by the setters' touches, {:.1f} only by the tags, {:.1f} changed;",
                       double(touch_marks) / double(census_ops), double(tag_only) / double(census_ops),
                       double(setter + inl) / double(census_ops));
  for (u32 b = 0; b < 18; ++b)
    if (const uint64_t n = g_census_setter_fields[b].exchange(0, std::memory_order_relaxed))
      out += fmt::format(" {} {}", kNames[b], n);
  out += "; inline by field:";
  for (u32 b = 0; b < 18; ++b)
    if (const uint64_t n = g_census_inline_fields[b].exchange(0, std::memory_order_relaxed))
      out += fmt::format(" {} {}", kNames[b], n);
  return out;
}

// banjo-nx (2026-10-27, banjo_renut_shadow_sort): the 10 s line (empty when nothing was planned or replayed).
std::string ShadowSortReport() {
  const uint64_t plans = g_sort_plans.exchange(0, std::memory_order_relaxed);
  if (!plans && !g_sort_replayed_runs && !g_sort_refused_runs)
    return {};
  const auto take = [](std::atomic<uint64_t> &a) { return a.exchange(0, std::memory_order_relaxed); };
  const uint64_t draws = take(g_sort_draws);
  uint64_t why[kSortWhyCount], len[6], end[6];
  for (u32 k = 0; k < kSortWhyCount; ++k)
    why[k] = take(g_sort_why[k]);
  for (u32 k = 0; k < 6; ++k) {
    len[k] = take(g_sort_lengths[k]);
    end[k] = take(g_sort_end[k]);
  }
  const double d = draws ? double(draws) : 1.0;
  std::string out = fmt::format(
      "shadow sort (banjo_renut_shadow_sort): {} recordings planned, {} draws: {:.1f} % sortable (not: off {}, the "
      "call {}, no depth target {}, stencil {}, colour written {}, depth test {}, shaders {}); runs of sortable draws by length: "
      "1-2 {}, 3-4 {}, 5-8 {}, 9-16 {}, 17-64 {}, more {}; ended by the recording's end {}, a run op {}, a clear or "
      "resolve {}, other targets {}, an unsortable draw {}; {} runs of 3+ ({} draws), {} sorted ({} draws): shader "
      "changes {} -> {}, {} slots copied; replayed sorted {} runs ({} draws), {} refused by their shaders; verify: {} "
      "devices compared, {} differ",
      plans, draws, 100.0 * double(why[kSortOk]) / d, why[kSortOff], why[kSortCall], why[kSortNoDepth],
      why[kSortStencil], why[kSortColor], why[kSortDepthFunc], why[kSortShader], len[0], len[1], len[2], len[3], len[4],
      len[5], end[0],
      end[1], end[2], end[3], end[4], take(g_sort_runs), take(g_sort_run_draws), take(g_sort_sorted_runs),
      take(g_sort_sorted_draws), take(g_sort_changes_before), take(g_sort_changes_after), take(g_sort_pairs),
      g_sort_replayed_runs, g_sort_replayed_draws, g_sort_refused_runs, g_sort_verified, g_sort_verify_differ);
  g_sort_replayed_runs = g_sort_replayed_draws = g_sort_refused_runs = 0;
  g_sort_verified = g_sort_verify_differ = 0;
  return out;
}

std::string TagsReport() {
  const uint64_t ops = g_tag_ops.exchange(0, std::memory_order_relaxed);
  const uint64_t marked_ops = g_tag_marked_ops.exchange(0, std::memory_order_relaxed);
  const uint64_t marked = g_tag_marked.exchange(0, std::memory_order_relaxed);
  const uint64_t changed = g_tag_changed.exchange(0, std::memory_order_relaxed);
  const uint64_t checks = g_tag_checks.exchange(0, std::memory_order_relaxed);
  const uint64_t unmarked = g_tag_unmarked.exchange(0, std::memory_order_relaxed);
  if (!ops)
    return {};
  // banjo-nx (2026-10-28, banjo_renut_record_lookups): the recording's object lookups (the devices' counts since the
  // last report).
  uint64_t hits_now = 0, tables_now = 0;
  for (const ResourceMemo &m : g_resource_memos) {
    hits_now += m.hits.load(std::memory_order_relaxed);
    tables_now += m.tables.load(std::memory_order_relaxed);
  }
  static uint64_t hits_was = 0, tables_was = 0;
  const uint64_t memo_hits = hits_now - hits_was, tables = tables_now - tables_was;
  hits_was = hits_now;
  tables_was = tables_now;
  return fmt::format("recording from marks (banjo_renut_tags): {} ops, {:.1f} % compared from the setters' marks, {:.1f} "
                     "chunks marked an op of those (of {}), {:.1f} chunks changed an op; {} whole checks, {} chunks "
                     "changed without a mark{}{}",
                     ops, 100.0 * double(marked_ops) / double(ops),
                     marked_ops ? double(marked) / double(marked_ops) : 0.0, kPlainChunks,
                     double(changed) / double(ops), checks, unmarked,
                     g_tags_failed.load(std::memory_order_relaxed) ? " (marks off: every chunk compared)" : "",
                     memo_hits || tables
                         ? fmt::format("; object lookups (banjo_renut_record_lookups): {} from the devices' memos, {} "
                                       "from the tables ({:.1f} % memo)",
                                       memo_hits, tables, 100.0 * double(memo_hits) / double(memo_hits + tables))
                         : std::string()) +
         (g_group_pointer_reads
              ? fmt::format("; group pointers verify (banjo_renut_replay_group_pointers_verify): {} reads, {} differ",
                            std::exchange(g_group_pointer_reads, 0), std::exchange(g_group_pointer_differ, 0))
              : std::string()) +
         // blur-nx (2026-11-13): banjo_renut_set_words_verify's counts (the ops checked in 10 s, the words missed in all)
         (g_set_words_checked.load(std::memory_order_relaxed)
              ? fmt::format("; set words by changed chunks verify (banjo_renut_set_words_verify): {} ops checked, {} "
                            "words missed since the start",
                            g_set_words_checked.exchange(0, std::memory_order_relaxed),
                            g_set_words_missed.load(std::memory_order_relaxed))
              : std::string());
}

CommandBufferMemory CommandBufferMemoryUsage() {
  CommandBufferMemory usage;
  {
    std::lock_guard lock(g_record_mutex);
    for (const auto &[va, buffer] : g_buffers) {
      if (buffer) {
        usage.recorded_bytes += CapacityBytes(*buffer);
        ++usage.recorded;
      }
    }
  }
  {
    std::lock_guard lock(g_free_mutex);
    usage.spare_bytes = g_free_bytes;
    usage.spare = g_free_buffers.size();
  }
  usage.live = g_live_buffers.load(std::memory_order_relaxed);
  return usage;
}

// sasrt-nx (2026-10-21, sasrt_renut_constant_f4_pending): see command_buffers.h. A group is 4 registers (D3DTAGCOLLECTION), bit
// 63 - group.
void KeepConstantF4Pending(u32 device_va, bool pixel, u32 start, u32 count) {
  if (!banjo::renut_gpu::ConstantF4PendingOn() || !count || start + count > 256)
    return;
  auto *d = GuestPtr<d3d::Device>(device_va);
  if (!d)
    return;
  uint64_t mask = 0;
  for (u32 g = start >> 2; g <= (start + count - 1) >> 2; ++g)
    mask |= uint64_t(1) << (63 - g);
  d->m_Pending[pixel ? 1 : 0] = u64(d->m_Pending[pixel ? 1 : 0]) | mask;
  static std::atomic<uint64_t> calls{0};
  if (calls.fetch_add(1, std::memory_order_relaxed) == 0)
    RGPU_INFO("[renut] D3DDevice_BeginShaderConstantF4 used (first: device {:08X}, {} shader, registers {}-{}, recording {}): "
              "its range kept pending (sasrt_renut_constant_f4_pending)", device_va, pixel ? "pixel" : "vertex", start,
              start + count - 1, IsRecordingCommandBuffer(device_va) ? "yes" : "no");
}

} // namespace renut::gpu
