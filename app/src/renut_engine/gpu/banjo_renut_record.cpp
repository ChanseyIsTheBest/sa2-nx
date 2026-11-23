// banjo-nx (package 28b): banjo_renut_record_worker. See banjo_renut_record.h.
#include "renut_engine/gpu/banjo_renut_record.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <plume_render_interface.h>
#include <plume_vulkan.h>
#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include "banjo_switch_default.h"  // banjo-nx (2026-11-21): the stage's Switch default
#include "cpu/banjo_enhancements.h"
#include "me/me_platform_compat.h"  // RexSwitchPinCurrentThreadToCore and the priority helper (no-ops off the Switch)
#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"

REXCVAR_DEFINE_BOOL(banjo_renut_record_worker, true, "Banjo/Enhancements",
                    "Renut's renderer (package 28b): its command recording split across two threads, as banjo-nx's "
                    "renderer's deferred recording: the render thread records into a proxy (each plume call copied "
                    "into a command stream) and a worker on another core plays it on the real command list in the "
                    "same order, so plume's and NVK's recording work (a quarter of the render thread on the console) "
                    "leaves the render thread. The list is ended and submitted once the worker has played all of it. "
                    "Same commands, same order. CPU group; in-run A/B target 14");
REXCVAR_DEFINE_INT32(banjo_renut_record_worker_core, 0, "Banjo",
                     "Renut's renderer, the Switch: the core of banjo_renut_record_worker's thread (-1: not pinned; "
                     "the render thread has banjo_ring_core, the pipeline workers banjo_background_compile_core)")
    .range(-1, 2);
// banjo-nx (2026-11-21, the user's "build core 3"): banjo_renut_record.h.
// banjo-nx (2026-11-21): on by default on the Switch after console runs c42 (A/B 182: render thread -5.0 % a draw,
// 68 of 68 pairs; fps +2.8 %) and c43 (batch 2 with it: 6.83 us a draw by day against 7.35). 4-core mode only.
REXCVAR_DEFINE_BOOL(banjo_deko3d_command_stage, BANJO_SWITCH_DEFAULT(true, false), "Banjo/Enhancements",
                    "deko3d renderer (2026-11-21, the core-3 command stage): the render thread records its command "
                    "list into a stream and a thread on the system core (core 3, 4-core mode only) plays it into "
                    "deko3d (plume's command list and libdeko3d's encoding move off the render thread). Same "
                    "commands, same order. In-run A/B target 182 (its off half: the render thread records directly)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_deko3d_command_stage_priority, 0x2E, "Banjo",
                     "deko3d command stage: its thread's priority on the system core (0x2E: below the game's audio "
                     "threads there, 0x2A-0x2D, above the shader compiler)")
    .range(0x20, 0x3F)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_deko3d_command_stage_batch, 1024, "Banjo",
                     "deko3d command stage: commands recorded before a hand-over to its thread (each a possible wake: "
                     "fewer, larger hand-overs cost the render thread less; the frame's last part waits for its play)")
    .range(64, 16384)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-11-21, the user's batch 2).
REXCVAR_DEFINE_INT32(banjo_deko3d_command_stage_poll_us, 200, "Banjo",
                     "deko3d command stage: with nothing to play its thread looks for work every this many microseconds "
                     "and the render thread never wakes it (each wake was a system call on the render thread, about 18 a "
                     "frame); 0: woken at each hand-over, as before. In-run A/B 186 (its off half: woken)")
    .range(0, 5000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_deko3d_command_stage_peek, 8, "Banjo",
                     "deko3d command stage: commands it reads ahead of the one it plays, prefetching the pipelines, "
                     "framebuffers and buffers they name (0: none). In-run A/B 187 (its off half: none)")
    .range(0, 64)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-11-21, batch 2).
// banjo-nx (2026-11-22, console run c44): A/B 188 gave the render thread +0.3 %, GPU +0.4 % (8 of 10 pairs higher),
// fps -0.7 %: off by default (0).
REXCVAR_DEFINE_INT32(banjo_deko3d_command_stage_stream_ahead, 0, "Banjo",
                     "deko3d command stage (2026-11-21, its ARM audit): the stream's line this many bytes past each "
                     "command is prefetched as the command is played (0: none). In-run A/B 188 (its off half: none)")
    .range(0, 4096)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-11-22, the user).
// banjo-nx (2026-11-22, console run c44): A/B 189 gave the render thread -1.5 % a draw (9 of 9 pairs lower) but all
// threads' CPU +2.5 % (9 of 9 higher) and fps -3.3 %: on cores 1-2 the stage and the warm helper took the game's
// threads' time. Off by default (core 3 alone, as before).
REXCVAR_DEFINE_BOOL(banjo_deko3d_command_stage_float, false, "Banjo",
                    "deko3d command stage (and the cache-warming helper): its thread may run on cores 1-3, core 3 (the "
                    "system core) its preferred one, so Horizon moves it to core 1 or 2 while core 3 is busy (the game's "
                    "audio threads and the system's outrank it there; it outranks the game's threads on cores 1-2, the "
                    "render thread outranks it on core 1). Off: on core 3 alone. In-run A/B 189 (its off half: core 3)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-11-21, batch 3).
REXCVAR_DEFINE_BOOL(banjo_deko3d_command_stage_notify_waiters, true, "Banjo",
                    "deko3d command stage: after each chunk it signals the render thread only when that thread waits for "
                    "it (a drain, a staged submit): a signal is a kernel call on the Switch even with nobody waiting, and "
                    "it made one after every chunk (about 18 a frame on core 3). Off: after every chunk, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_deko3d_stage_retired_free, true, "Banjo",
                    "deko3d command stage: the objects retired with a frame slot go at its DrainSlot without draining "
                    "the stage (it has played every command of that frame: the slot's staged submit was waited for). "
                    "Off: each such destructor waits for the stage to play all it has (with the frame-end drain gone "
                    "(banjo_deko3d_stage_submit) that was 20-300 waits in 10 s in the emulator)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_deko3d_command_stage_any_core, false, "Banjo/Diagnostics",
                    "deko3d command stage without 4-core mode (the emulator's tests): its thread on any core, not pinned")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_deko3d_command_stage_verify, false, "Banjo/Diagnostics",
                    "deko3d command stage: before every recorded draw a hash of the render thread's shared constants "
                    "copy (what bindTextures reads) goes into the stream, and the stage compares its own copy's (the "
                    "10 s line's 'shared copies checked / differ')")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, banjo_cpu_verify);

#if REX_PLATFORM_SWITCH
extern "C" int RexSwitchSystemCore(void);
extern "C" bool RexSwitchMoveCurrentThreadToSystemCore(int priority);
#endif

namespace banjo::renut_gpu::record {
namespace {

constexpr int kWorkerPriority = 0x2D;          // just below the render thread (0x2C), above the game's threads (0x3B)
constexpr size_t kChunkBytes = 256u << 10;     // a chunk of the stream (a command never spans two)
constexpr size_t kPublishBytes = 64u << 10;    // handed to the worker once this much is recorded...
constexpr uint32_t kPublishCommands = 384;     // ...or this many commands (so it starts early in each list; the PC
                                               // run at 96 woke the worker about 70 times a frame)

enum Op : uint16_t {
  kBegin, kEnd, kBarriers, kDispatch, kDraw, kDrawIndexed, kSetPipeline, kSetComputeLayout, kSetComputePush,
  kSetComputeSet, kSetGraphicsLayout, kSetGraphicsPush, kSetGraphicsSet, kSetGraphicsSetDynamic, kSetRootDescriptor,
  kSetIndexBuffer, kSetVertexBuffers, kSetViewports, kSetScissors, kSetFramebuffer, kSetDepthBias,
  kSetStencilReference, kClearColor, kClearDepthStencil, kCopyBufferRegion, kCopyTextureRegion, kCopyBuffer,
  kCopyTexture, kResolveTexture, kResolveTextureRegion, kDiscardTexture, kResetQueryPool, kWriteTimestamp,
  kBlendConstants,
  // banjo-nx (2026-11-21, the deko3d command stage): plume's deko3d calls that take the list, and the image tables.
  kD3PushUniform, kD3BindUniform, kD3BindStorage, kD3SetSharedWords, kD3SharedLines, kD3BindFragTextures,
  kD3BindMemExport, kD3BindFetchData, kD3FlushL2, kD3DiscardNextDraw, kD3SampleMask, kD3NextPrimitive, kD3BufferWait,
  kD3ReportCounter, kD3ImageTrack, kD3VerifyShared,
  kStageCall,  // banjo-nx (2026-11-21, item 5): StageCall
  kD3PushUniformRaw,  // banjo-nx (2026-11-21, item 3): detail::PushUniformRawBegin
  kOpCount
};

struct Header {
  uint16_t op;
  uint16_t pad;
  uint32_t size;  // the whole command, a multiple of 8
};

template <typename T>
constexpr bool kPod = std::is_trivially_copyable_v<T>;
static_assert(kPod<plume::RenderBufferBarrier> && kPod<plume::RenderTextureBarrier> &&
              kPod<plume::RenderTextureCopyLocation> && kPod<plume::RenderVertexBufferView> &&
              kPod<plume::RenderInputSlot> && kPod<plume::RenderIndexBufferView> && kPod<plume::RenderViewport> &&
              kPod<plume::RenderRect> && kPod<plume::RenderBox> && kPod<plume::RenderColor> &&
              kPod<plume::RenderBufferReference>, "the proxy copies these as bytes");

struct Chunk {
  std::unique_ptr<uint8_t[]> bytes{new uint8_t[kChunkBytes]};
  size_t used = 0;
  plume::RenderCommandList *target = nullptr;  // the real list the chunk's commands are for
};

// Render thread -> worker.
std::mutex g_mutex;
std::condition_variable g_work;   // a chunk queued, or stopping
std::condition_variable g_done;   // a chunk played
// banjo-nx (2026-11-21, batch 3, banjo_deko3d_command_stage_notify_waiters): threads waiting on g_done (under g_mutex).
int g_done_waiters = 0;
std::deque<Chunk *> g_queue;
std::vector<Chunk *> g_free;
bool g_worker_waiting = false;
bool g_stop = false;
bool g_started = false;
bool g_exited = false;  // the worker returned (after a stop with everything played)
uint64_t g_published = 0;  // chunks handed over (render thread, under g_mutex)
uint64_t g_played = 0;     // chunks played (worker, under g_mutex)

// Statistics.
std::atomic<uint64_t> g_commands{0}, g_chunks{0}, g_drains{0}, g_drain_wait_us{0}, g_worker_busy_us{0}, g_lists{0},
    g_direct_lists{0}, g_wakes{0};

bool On() {
  static const bool on = EnhancementActive(EnhancementGroup::kCpu, REXCVAR_GET(banjo_renut_record_worker));
  return on && RecordWorkerAbOn();
}
// banjo-nx (2026-11-21): the deko3d command stage, in 4-core mode only (on the game's cores it would take their time).
bool StageOn() {
#if defined(PLUME_DEKO3D) && REX_PLATFORM_SWITCH
  static const bool on = REXCVAR_GET(banjo_deko3d_command_stage) &&
                         (RexSwitchSystemCore() >= 0 || REXCVAR_GET(banjo_deko3d_command_stage_any_core));
  return on && g_ab_off.load(std::memory_order_relaxed) != 182;
#elif defined(PLUME_DEKO3D)
  static const bool on = REXCVAR_GET(banjo_deko3d_command_stage) && REXCVAR_GET(banjo_deko3d_command_stage_any_core);
  return on && g_ab_off.load(std::memory_order_relaxed) != 182;
#else
  return false;
#endif
}
// The shared constants' copy's hash (banjo_deko3d_command_stage_verify): FNV-1a over 64-bit words.
uint64_t SharedHash(const uint8_t *p, uint32_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  uint32_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = (h ^ w) * 0x100000001B3ull;
  }
  for (; i < n; ++i)
    h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}
std::atomic<uint64_t> g_shared_checked{0}, g_shared_differ{0};
bool g_deko3d_stage = false;  // the worker plays deko3d lists (set before it starts)
thread_local bool t_recording_thread = false;  // the render thread, once it has wrapped a list

void Play(plume::RenderCommandList *cmd, const uint8_t *p, const uint8_t *end);

void Worker() {
#if REX_PLATFORM_SWITCH
  if (g_deko3d_stage) {
    const int priority = REXCVAR_GET(banjo_deko3d_command_stage_priority);
    const bool moved = RexSwitchMoveCurrentThreadToSystemCore(priority);
    REXLOG_INFO("[renut] deko3d command stage: thread on the system core {}{} at priority 0x{:X}", RexSwitchSystemCore(),
                moved ? "" : " (NOT accepted)", priority);
  } else {
  const int core = REXCVAR_GET(banjo_renut_record_worker_core);
  const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(kWorkerPriority);
  const bool core_ok = core < 0 || RexSwitchPinCurrentThreadToCore(core);
  REXLOG_INFO("[renut] record worker at priority 0x{:X}{}, {}", kWorkerPriority, priority_ok ? "" : " (NOT accepted)",
              core < 0 ? std::string("not pinned") : fmt::format("core {}{}", core, core_ok ? "" : " (NOT accepted)"));
  }
#endif
  static const int poll_us = REXCVAR_GET(banjo_deko3d_command_stage_poll_us);
  static const bool notify_waiters_only = REXCVAR_GET(banjo_deko3d_command_stage_notify_waiters);
  int cores_applied = g_deko3d_stage ? 1 : 0;  // (moved to core 3 above; the record worker on Vulkan: not followed)
  for (;;) {
    if (g_deko3d_stage)
      FollowStageCores(cores_applied);  // banjo-nx (2026-11-22, A/B 189)
    Chunk *c = nullptr;
    {
      std::unique_lock lock(g_mutex);
      while (g_queue.empty() && !g_stop) {
        // banjo-nx (2026-11-21, batch 2): the stage looks for work every poll_us instead of being woken (A/B 186's off
        // half, and the record worker on Vulkan: woken).
        if (g_deko3d_stage && poll_us > 0 && g_ab_off.load(std::memory_order_relaxed) != 186) {
          lock.unlock();
          std::this_thread::sleep_for(std::chrono::microseconds(poll_us));
          lock.lock();
          continue;
        }
        g_worker_waiting = true;
        g_work.wait(lock);
        g_worker_waiting = false;
      }
      if (g_queue.empty()) {
        g_exited = true;  // stopping, and everything is played
        g_done.notify_all();
        return;
      }
      c = g_queue.front();
      g_queue.pop_front();
    }
    const auto t0 = std::chrono::steady_clock::now();
    bool wake = false;
    Play(c->target, c->bytes.get(), c->bytes.get() + c->used);
    g_worker_busy_us.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count()),
        std::memory_order_relaxed);
    {
      std::lock_guard lock(g_mutex);
      c->used = 0;
      g_free.push_back(c);
      ++g_played;
      wake = g_done_waiters > 0 || !notify_waiters_only;
    }
    if (wake)
      g_done.notify_all();
  }
}

template <typename T>
const T &At(const uint8_t *&p) {
  const T &v = *reinterpret_cast<const T *>(p);
  p += (sizeof(T) + 7) & ~size_t(7);
  return v;
}

template <typename T>
const T *Array(const uint8_t *&p, uint32_t count) {
  const T *v = count ? reinterpret_cast<const T *>(p) : nullptr;
  p += (sizeof(T) * count + 7) & ~size_t(7);
  return v;
}

struct Barriers { plume::RenderBarrierStages stages; uint32_t buffers, textures; };
struct U32x4 { uint32_t a, b, c, d; };
struct DrawIndexed { uint32_t count, instances, first; int32_t base; uint32_t first_instance; };
struct Ptr { const void *p; };
struct PushHead { uint32_t range, offset, size; };
struct SetHead { void *set; uint32_t index, count; };
struct RootHead { plume::RenderBufferReference ref; uint32_t index; };
struct VbHead { uint32_t start, count, has_slots; };
struct CountHead { uint32_t count; };
struct DepthBias { float bias, clamp, slope; };
struct ClearColorHead { uint32_t attachment; plume::RenderColor color; uint32_t rects; };
struct ClearDepthHead { uint32_t depth, stencil; float value; uint32_t stencil_value, rects; };
struct CopyBufferRegion { plume::RenderBufferReference dst, src; uint64_t size; };
struct CopyTextureRegionHead { plume::RenderTextureCopyLocation dst, src; uint32_t x, y, z, has_box; };
struct TwoPtr { const void *a, *b; };
struct ResolveRegionHead { const plume::RenderTexture *dst; uint32_t x, y; const plume::RenderTexture *src;
                           uint32_t has_rect; plume::RenderResolveMode mode; };
struct QueryHead { const plume::RenderQueryPool *pool; uint32_t first, count; };
struct Floats4 { float v[4]; };
// banjo-nx (2026-11-21, the deko3d command stage)
struct PushUniformHead { uint64_t address; uint32_t ubo_size, offset, size, pad; };
struct BindBufHead { uint64_t address; uint32_t slot, size; };
struct SharedWordsHead { uint32_t size, has; };
struct FragTexHead { uint32_t first, count; };
struct FetchHead { uint64_t address; uint32_t size, pad; };
struct BufferWaitHead { uint32_t invalidate, counts; };
struct ReportHead { const void *pool; uint32_t index, counter; };
struct VerifyHead { uint64_t hash; uint32_t size, pad; };
struct CallHead { void (*fn)(const void *); uint64_t serial; uint32_t size, pad; };
// banjo-nx (2026-11-21, item 3): the guest's big-endian words swapped in place (the chunk is the stage's while it plays),
// and with 'nan_flush' a word whose exponent is all ones zeroed (CopyAndSwapGroup's rule). 'n' a multiple of 16.
void SwapWordsInPlace(uint8_t *p, uint32_t n, bool nan_flush) {
#if defined(__aarch64__)
  const uint32x4_t exponent = vdupq_n_u32(0x7F800000u);
  for (uint32_t i = 0; i + 16 <= n; i += 16) {
    uint32x4_t w = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + i)));
    if (nan_flush)
      w = vbicq_u32(w, vceqq_u32(vandq_u32(w, exponent), exponent));
    vst1q_u8(p + i, vreinterpretq_u8_u32(w));
  }
#else
  for (uint32_t i = 0; i + 4 <= n; i += 4) {
    uint32_t w;
    std::memcpy(&w, p + i, 4);
    w = __builtin_bswap32(w);
    if (nan_flush && (w & 0x7F800000u) == 0x7F800000u)
      w = 0;
    std::memcpy(p + i, &w, 4);
  }
#endif
}
std::atomic<uint64_t> g_calls_done{0};  // banjo-nx (2026-11-21, item 5): the last StageCall the stage made
#if defined(PLUME_DEKO3D)
static_assert(kPod<plume::Deko3DImageTrack>, "the stream copies an image track as bytes");
// The stage's own copy of the shared constants that plume's bindTextures reads (the render thread's, g_bound.pushed_shared,
// is the next draws' by the time a draw is played): set whole at each recorded Deko3DSetSharedWords, then every recorded
// push to the shared buffer (uniform slot 2) written into it at its offset, as the render thread writes its copy after
// each push. Worker only.
std::vector<uint8_t> g_stage_shared;
uint64_t g_stage_shared_address = 0;
constexpr uint32_t kSharedUboSlot = 2;  // plume_deko3d_commands.cpp SharedUboSlot
#endif

// banjo-nx (2026-11-21, batch 2): the objects a command names, prefetched while the commands before it are played (the
// stage's setPipeline was 14.5 % of its thread in console run c42, most of it the pipeline object's first lines).
void PeekPrefetch(const uint8_t *q) {
  const Header &h = *reinterpret_cast<const Header *>(q);
  const uint8_t *payload = q + sizeof(Header);
  switch (Op(h.op)) {
  case kSetPipeline: {
    const char *pp = static_cast<const char *>(reinterpret_cast<const Ptr *>(payload)->p);
    if (pp) {
      __builtin_prefetch(pp);
      __builtin_prefetch(pp + 64);
      __builtin_prefetch(pp + 128);
      __builtin_prefetch(pp + 192);
    }
    break;
  }
  case kSetFramebuffer:
  case kD3BindMemExport: {
    const char *pp = static_cast<const char *>(reinterpret_cast<const Ptr *>(payload)->p);
    if (pp) {
      __builtin_prefetch(pp);
      __builtin_prefetch(pp + 64);
    }
    break;
  }
  case kSetIndexBuffer: {
    plume::RenderIndexBufferView v;
    std::memcpy(&v, payload, sizeof(v));
    if (v.buffer.ref)
      __builtin_prefetch(v.buffer.ref);
    break;
  }
  default:
    break;
  }
}

void Play(plume::RenderCommandList *cmd, const uint8_t *p, const uint8_t *end) {
  static const uint32_t peek = uint32_t(REXCVAR_GET(banjo_deko3d_command_stage_peek));
  const uint32_t ahead_max = g_deko3d_stage && g_ab_off.load(std::memory_order_relaxed) != 187 ? peek : 0u;
  // banjo-nx (2026-11-21, the stage's ARM audit, banjo_deko3d_command_stage_stream_ahead): the stream's lines were
  // written on the render thread's core and missed at their first load here (console run c42: a command's header and
  // its payload's first word were 13 % of the stage's samples). The line 'stream_ahead' bytes past each command is
  // prefetched: its address needs no load (the peek cursor's headers do), so the misses overlap the playing. PRFM
  // never faults (past the chunk's end too). 0: none; A/B 188's off half: none.
  static const uint32_t stream_ahead = uint32_t(REXCVAR_GET(banjo_deko3d_command_stage_stream_ahead));
  const uint32_t pf = g_deko3d_stage && g_ab_off.load(std::memory_order_relaxed) != 188 ? stream_ahead : 0u;
  const uint8_t *q = p;  // the peek cursor: the next command not peeked yet
  uint32_t ahead = 0;
  while (p < end) {
    while (ahead < ahead_max && q < end) {
      PeekPrefetch(q);
      q += reinterpret_cast<const Header *>(q)->size;
      ++ahead;
    }
    if (ahead)
      --ahead;
    else
      q = p + reinterpret_cast<const Header *>(p)->size;  // (peek off: the cursor kept past this command)
    if (pf)
      __builtin_prefetch(p + pf);
    const Header &h = *reinterpret_cast<const Header *>(p);
    const uint8_t *next = p + h.size;
    p += sizeof(Header);
    switch (Op(h.op)) {
    case kBegin: cmd->begin(); break;
    case kEnd: cmd->end(); break;
    case kBarriers: {
      const Barriers &b = At<Barriers>(p);
      const auto *bb = Array<plume::RenderBufferBarrier>(p, b.buffers);
      const auto *tb = Array<plume::RenderTextureBarrier>(p, b.textures);
      cmd->barriers(b.stages, bb, b.buffers, tb, b.textures);
      break;
    }
    case kDispatch: { const U32x4 &d = At<U32x4>(p); cmd->dispatch(d.a, d.b, d.c); break; }
    case kDraw: { const U32x4 &d = At<U32x4>(p); cmd->drawInstanced(d.a, d.b, d.c, d.d); break; }
    case kDrawIndexed: {
      const DrawIndexed &d = At<DrawIndexed>(p);
      cmd->drawIndexedInstanced(d.count, d.instances, d.first, d.base, d.first_instance);
      break;
    }
    case kSetPipeline: {
      auto *pipeline = static_cast<const plume::RenderPipeline *>(At<Ptr>(p).p);
#if !defined(PLUME_DEKO3D)
      // banjo_renut_tuning's NVK prefetch hint (the Switch), on the core that binds.
      // banjo-nx (2026-11-21, the stage's ARM audit): Vulkan's only (NVK): not on deko3d (it tested IsVulkan per bind).
      banjo::renut_gpu::BeforePipelineBind(const_cast<plume::RenderPipeline *>(pipeline));
#endif
      cmd->setPipeline(pipeline);
      break;
    }
    case kSetComputeLayout:
      cmd->setComputePipelineLayout(static_cast<const plume::RenderPipelineLayout *>(At<Ptr>(p).p));
      break;
    case kSetGraphicsLayout:
      cmd->setGraphicsPipelineLayout(static_cast<const plume::RenderPipelineLayout *>(At<Ptr>(p).p));
      break;
    case kSetComputePush:
    case kSetGraphicsPush: {
      const PushHead &ph = At<PushHead>(p);
      const uint8_t *data = Array<uint8_t>(p, ph.size);
      if (h.op == kSetComputePush)
        cmd->setComputePushConstants(ph.range, data, ph.offset, ph.size);
      else
        cmd->setGraphicsPushConstants(ph.range, data, ph.offset, ph.size);
      break;
    }
    case kSetComputeSet:
    case kSetGraphicsSet: {
      const SetHead &s = At<SetHead>(p);
      auto *set = static_cast<plume::RenderDescriptorSet *>(s.set);
      if (h.op == kSetComputeSet)
        cmd->setComputeDescriptorSet(set, s.index);
      else
        cmd->setGraphicsDescriptorSet(set, s.index);
      break;
    }
    case kSetGraphicsSetDynamic: {
      const SetHead &s = At<SetHead>(p);
      const uint32_t *offsets = Array<uint32_t>(p, s.count);
      cmd->setGraphicsDescriptorSetDynamic(static_cast<plume::RenderDescriptorSet *>(s.set), s.index, offsets, s.count);
      break;
    }
    case kSetRootDescriptor: { const RootHead &r = At<RootHead>(p); cmd->setGraphicsRootDescriptor(r.ref, r.index); break; }
    case kSetIndexBuffer: {
      const auto &v = At<plume::RenderIndexBufferView>(p);
      cmd->setIndexBuffer(&v);
      break;
    }
    case kSetVertexBuffers: {
      const VbHead &v = At<VbHead>(p);
      const auto *views = Array<plume::RenderVertexBufferView>(p, v.count);
      const auto *slots = v.has_slots ? Array<plume::RenderInputSlot>(p, v.count) : nullptr;
      cmd->setVertexBuffers(v.start, views, v.count, slots);
      break;
    }
    case kSetViewports: {
      const uint32_t n = At<CountHead>(p).count;
      cmd->setViewports(Array<plume::RenderViewport>(p, n), n);
      break;
    }
    case kSetScissors: {
      const uint32_t n = At<CountHead>(p).count;
      cmd->setScissors(Array<plume::RenderRect>(p, n), n);
      break;
    }
    case kSetFramebuffer: cmd->setFramebuffer(static_cast<const plume::RenderFramebuffer *>(At<Ptr>(p).p)); break;
    case kSetDepthBias: { const DepthBias &d = At<DepthBias>(p); cmd->setDepthBias(d.bias, d.clamp, d.slope); break; }
    case kSetStencilReference: cmd->setStencilReference(At<CountHead>(p).count); break;
    case kClearColor: {
      const ClearColorHead &c = At<ClearColorHead>(p);
      cmd->clearColor(c.attachment, c.color, Array<plume::RenderRect>(p, c.rects), c.rects);
      break;
    }
    case kClearDepthStencil: {
      const ClearDepthHead &c = At<ClearDepthHead>(p);
      cmd->clearDepthStencil(c.depth != 0, c.stencil != 0, c.value, c.stencil_value,
                             Array<plume::RenderRect>(p, c.rects), c.rects);
      break;
    }
    case kCopyBufferRegion: {
      const CopyBufferRegion &c = At<CopyBufferRegion>(p);
      cmd->copyBufferRegion(c.dst, c.src, c.size);
      break;
    }
    case kCopyTextureRegion: {
      const CopyTextureRegionHead &c = At<CopyTextureRegionHead>(p);
      const plume::RenderBox *box = c.has_box ? &At<plume::RenderBox>(p) : nullptr;
      cmd->copyTextureRegion(c.dst, c.src, c.x, c.y, c.z, box);
      break;
    }
    case kCopyBuffer: {
      const TwoPtr &t = At<TwoPtr>(p);
      cmd->copyBuffer(static_cast<const plume::RenderBuffer *>(t.a), static_cast<const plume::RenderBuffer *>(t.b));
      break;
    }
    case kCopyTexture:
    case kResolveTexture: {
      const TwoPtr &t = At<TwoPtr>(p);
      const auto *a = static_cast<const plume::RenderTexture *>(t.a);
      const auto *b = static_cast<const plume::RenderTexture *>(t.b);
      if (h.op == kCopyTexture)
        cmd->copyTexture(a, b);
      else
        cmd->resolveTexture(a, b);
      break;
    }
    case kResolveTextureRegion: {
      const ResolveRegionHead &r = At<ResolveRegionHead>(p);
      const plume::RenderRect *rect = r.has_rect ? &At<plume::RenderRect>(p) : nullptr;
      cmd->resolveTextureRegion(r.dst, r.x, r.y, r.src, rect, r.mode);
      break;
    }
    case kDiscardTexture: cmd->discardTexture(static_cast<const plume::RenderTexture *>(At<Ptr>(p).p)); break;
    case kResetQueryPool: { const QueryHead &q = At<QueryHead>(p); cmd->resetQueryPool(q.pool, q.first, q.count); break; }
    case kWriteTimestamp: { const QueryHead &q = At<QueryHead>(p); cmd->writeTimestamp(q.pool, q.first); break; }
    case kBlendConstants: {
      const Floats4 &f = At<Floats4>(p);
#if defined(PLUME_DEKO3D)
      if (g_deko3d_stage) {
        plume::Deko3DSetBlendConstant(cmd, f.v);
        break;
      }
#endif
      VkCommandBuffer vk = static_cast<plume::VulkanCommandList *>(cmd)->vk;
      if (vk && vkCmdSetBlendConstants)
        vkCmdSetBlendConstants(vk, f.v);
      break;
    }
#if defined(PLUME_DEKO3D)
    case kD3PushUniform: {
      const PushUniformHead &u = At<PushUniformHead>(p);
      const uint8_t *data = Array<uint8_t>(p, u.size);
      plume::Deko3DPushUniform(cmd, u.address, u.ubo_size, u.offset, data, u.size);
      if (u.address == g_stage_shared_address && uint64_t(u.offset) + u.size <= g_stage_shared.size())
        std::memcpy(g_stage_shared.data() + u.offset, data, u.size);
      break;
    }
    case kD3BindUniform: {
      const BindBufHead &b = At<BindBufHead>(p);
      plume::Deko3DBindUniform(cmd, b.slot, b.address, b.size);
      if (b.slot == kSharedUboSlot)
        g_stage_shared_address = b.address;
      break;
    }
    case kD3BindStorage: {
      const BindBufHead &b = At<BindBufHead>(p);
      plume::Deko3DBindStorage(cmd, b.slot, b.address, b.size);
      break;
    }
    case kD3SetSharedWords: {
      const SharedWordsHead &w = At<SharedWordsHead>(p);
      if (w.has) {
        const uint8_t *bytes = Array<uint8_t>(p, w.size);
        g_stage_shared.assign(bytes, bytes + w.size);
        plume::Deko3DSetSharedWords(cmd, g_stage_shared.data(), w.size);
      } else {
        plume::Deko3DSetSharedWords(cmd, nullptr, 0);
      }
      break;
    }
    case kD3SharedLines: plume::Deko3DSharedLinesWritten(cmd, At<CountHead>(p).count); break;
    case kD3BindFragTextures: {
      const FragTexHead &f = At<FragTexHead>(p);
      plume::Deko3DBindFragmentTextures(cmd, f.first, Array<uint32_t>(p, f.count), f.count);
      break;
    }
    case kD3BindMemExport:
      plume::Deko3DBindMemExport(cmd, static_cast<const plume::RenderBuffer *>(At<Ptr>(p).p));
      break;
    case kD3BindFetchData: {
      const FetchHead &f = At<FetchHead>(p);
      plume::Deko3DBindFetchData(cmd, f.address, f.size);
      break;
    }
    case kD3FlushL2: plume::Deko3DFlushL2(cmd); break;
    case kD3DiscardNextDraw: plume::Deko3DDiscardNextDraw(cmd); break;
    case kD3SampleMask: plume::Deko3DSetSampleMask(cmd, At<CountHead>(p).count); break;
    case kD3NextPrimitive: plume::Deko3DSetNextPrimitive(cmd, At<CountHead>(p).count); break;
    case kD3BufferWait: {
      const BufferWaitHead &w = At<BufferWaitHead>(p);
      plume::Deko3DBufferWait(cmd, w.invalidate, w.counts != 0);
      break;
    }
    case kD3ReportCounter: {
      const ReportHead &r = At<ReportHead>(p);
      plume::Deko3DReportCounter(cmd, static_cast<const plume::RenderQueryPool *>(r.pool), r.index, r.counter);
      break;
    }
    case kD3ImageTrack: plume::Deko3DApplyImageTrack(At<plume::Deko3DImageTrack>(p)); break;
    case kD3PushUniformRaw: {
      const PushUniformHead &u = At<PushUniformHead>(p);
      uint8_t *data = const_cast<uint8_t *>(Array<uint8_t>(p, u.size));
      SwapWordsInPlace(data, u.size, u.pad != 0);
      plume::Deko3DPushUniform(cmd, u.address, u.ubo_size, u.offset, data, u.size);
      break;
    }
    case kStageCall: {
      const CallHead &call = At<CallHead>(p);
      const uint8_t *args = Array<uint8_t>(p, call.size);
      call.fn(args);
      g_calls_done.store(call.serial, std::memory_order_release);
      break;
    }
    case kD3VerifyShared: {
      const VerifyHead &v = At<VerifyHead>(p);
      g_shared_checked.fetch_add(1, std::memory_order_relaxed);
      if (v.size != g_stage_shared.size() || SharedHash(g_stage_shared.data(), v.size) != v.hash) {
        if (g_shared_differ.fetch_add(1, std::memory_order_relaxed) < 8)
          REXLOG_WARN("[renut] deko3d command stage verify: the stage's shared constants copy differs from the render "
                      "thread's at a draw ({} bytes against {})", g_stage_shared.size(), v.size);
      }
      break;
    }
#endif
    default:
      REXLOG_ERROR("[renut] record worker: unknown command {}", h.op);
      return;
    }
    p = next;
  }
}

// The proxy. Render thread only.
class Recorder final : public plume::RenderCommandList {
public:
  plume::RenderCommandList *real = nullptr;

  // Makes room for a command of 'payload' bytes and returns where its payload goes.
  // banjo-nx (2026-11-21, batch 3's recorder trim): the write pointer, the chunk's end and its hand-over point are the
  // recorder's (the chunk's 'used' is written at Publish), and whether this command reaches the hand-over is decided
  // here, before the payload's stores (after them Done loaded the chunk, its 'used' and the counts again: a byte store
  // may alias them).
  uint8_t *Emit(Op op, size_t payload) {
    const size_t size = sizeof(Header) + ((payload + 7) & ~size_t(7));
    uint8_t *p = wp_;
    if (__builtin_expect(size_t(end_ - p) < size, 0)) {  // (no chunk yet: both null)
      Publish(true);
      p = wp_;
    }
    uint8_t *const next = p + size;
    wp_ = next;
    const uint32_t n = ++commands_;
    due_ = next >= publish_at_ || n >= publish_commands_;
    Header &h = *reinterpret_cast<Header *>(p);
    h.op = op;
    h.pad = 0;
    h.size = uint32_t(size);
    return p + sizeof(Header);
  }

  template <typename T>
  static uint8_t *Put(uint8_t *p, const T &v) {
    std::memcpy(p, &v, sizeof(T));
    return p + ((sizeof(T) + 7) & ~size_t(7));
  }
  template <typename T>
  static uint8_t *PutArray(uint8_t *p, const T *v, uint32_t n) {
    if (n)
      std::memcpy(p, v, sizeof(T) * n);
    return p + ((sizeof(T) * n + 7) & ~size_t(7));
  }
  template <typename T>
  static constexpr size_t Size(uint32_t n = 1) {
    return (sizeof(T) * n + 7) & ~size_t(7);
  }

  // Hands the recorded chunk to the worker (always when 'full'; else once enough is recorded).
  void Publish(bool full) {
    if (cur_)
      cur_->used = size_t(wp_ - cur_->bytes.get());  // (batch 3: the write pointer is the recorder's)
    if (cur_ && cur_->used && (full || cur_->used >= kPublishBytes || commands_ >= publish_commands_)) {
      Chunk *c = cur_;
      cur_ = nullptr;
      c->target = real;  // every command since the last hand-over is for the list open now (Wrap)
      bool wake;
      {
        std::lock_guard lock(g_mutex);
        g_queue.push_back(c);
        ++g_published;
        wake = g_worker_waiting;
      }
      if (wake) {
        g_work.notify_one();
        g_wakes.fetch_add(1, std::memory_order_relaxed);
      }
      g_chunks.fetch_add(1, std::memory_order_relaxed);
      g_commands.fetch_add(commands_, std::memory_order_relaxed);
      commands_ = 0;
    }
    if (!cur_) {
      std::lock_guard lock(g_mutex);
      if (!g_free.empty()) {
        cur_ = g_free.back();
        g_free.pop_back();
      }
    }
    if (!cur_)
      cur_ = new Chunk();
    uint8_t *const base = cur_->bytes.get();
    wp_ = base + cur_->used;
    end_ = base + kChunkBytes;
    publish_at_ = base + kPublishBytes;
    due_ = false;
  }

  void Drain() {
    Publish(true);
    const auto t0 = std::chrono::steady_clock::now();
    {
      std::unique_lock lock(g_mutex);
      if (g_played != g_published) {
        ::renut::gpu::render_thread::WaitScope wait;
        ++g_done_waiters;
        g_done.wait(lock, [] { return g_played == g_published; });
        --g_done_waiters;
      }
    }
    g_drains.fetch_add(1, std::memory_order_relaxed);
    g_drain_wait_us.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count()),
        std::memory_order_relaxed);
  }

  // A command ends; it goes out once the chunk has enough (the worker starts early in a list).
  void Done() {
    if (due_)
      Publish(false);
  }

  void begin() override {
    // banjo-nx (2026-11-21): plume's begin() forgets the list's shared constants copy (Deko3DSetSharedWords): so does the
    // verify form (the renderer's own copy is zeroed with the new list, ResetBound, and set again at its first pushed
    // draw; a draw before that binds its shared constants from the ring). Emulator run stage_verify: 6 "differ" were
    // such draws, at the start of a list.
    shared_cpu_ = nullptr;
    shared_size_ = 0;
    Emit(kBegin, 0);
    Done();
  }
  void end() override { Emit(kEnd, 0); Done(); }
  void barriers(plume::RenderBarrierStages stages, const plume::RenderBufferBarrier *bb, uint32_t bn,
                const plume::RenderTextureBarrier *tb, uint32_t tn) override {
    uint8_t *p = Emit(kBarriers, Size<Barriers>() + Size<plume::RenderBufferBarrier>(bn) +
                                     Size<plume::RenderTextureBarrier>(tn));
    p = Put(p, Barriers{stages, bn, tn});
    p = PutArray(p, bb, bn);
    PutArray(p, tb, tn);
    if (bn)
      ++buffer_waits_;  // banjo-nx (2026-11-21): plume's Deko3DBufferWaits counts these
    Done();
  }
  void dispatch(uint32_t x, uint32_t y, uint32_t z) override {
    Put(Emit(kDispatch, Size<U32x4>()), U32x4{x, y, z, 0});
    Done();
  }
  void traceRays(uint32_t, uint32_t, uint32_t, plume::RenderBufferReference,
                 const plume::RenderShaderBindingGroupsInfo &) override {
    Unsupported("traceRays");
  }
  void drawInstanced(uint32_t a, uint32_t b, uint32_t c, uint32_t d) override {
    VerifyShared();  // banjo-nx (2026-11-21): banjo_deko3d_command_stage_verify
    Put(Emit(kDraw, Size<U32x4>()), U32x4{a, b, c, d});
    Done();
  }
  void drawIndexedInstanced(uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e) override {
    VerifyShared();  // banjo-nx (2026-11-21): banjo_deko3d_command_stage_verify
    // (batch 3: stored in place, not built on the stack and copied)
    *reinterpret_cast<DrawIndexed *>(Emit(kDrawIndexed, Size<DrawIndexed>())) = DrawIndexed{a, b, c, d, e};
    Done();
  }
  void setPipeline(const plume::RenderPipeline *pipeline) override { PutPtr(kSetPipeline, pipeline); }
  void setComputePipelineLayout(const plume::RenderPipelineLayout *l) override { PutPtr(kSetComputeLayout, l); }
  void setComputePushConstants(uint32_t range, const void *data, uint32_t offset, uint32_t size) override {
    Push(kSetComputePush, range, data, offset, size);
  }
  void setComputeDescriptorSet(plume::RenderDescriptorSet *set, uint32_t index) override {
    Put(Emit(kSetComputeSet, Size<SetHead>()), SetHead{set, index, 0});
    Done();
  }
  void setGraphicsPipelineLayout(const plume::RenderPipelineLayout *l) override { PutPtr(kSetGraphicsLayout, l); }
  void setGraphicsPushConstants(uint32_t range, const void *data, uint32_t offset, uint32_t size) override {
    Push(kSetGraphicsPush, range, data, offset, size);
  }
  void setGraphicsDescriptorSet(plume::RenderDescriptorSet *set, uint32_t index) override {
    Put(Emit(kSetGraphicsSet, Size<SetHead>()), SetHead{set, index, 0});
    Done();
  }
  void setGraphicsDescriptorSetDynamic(plume::RenderDescriptorSet *set, uint32_t index, const uint32_t *offsets,
                                       uint32_t count) override {
    uint8_t *p = Emit(kSetGraphicsSetDynamic, Size<SetHead>() + Size<uint32_t>(count));
    p = Put(p, SetHead{set, index, count});
    PutArray(p, offsets, count);
    Done();
  }
  void setGraphicsRootDescriptor(plume::RenderBufferReference ref, uint32_t index) override {
    Put(Emit(kSetRootDescriptor, Size<RootHead>()), RootHead{ref, index});
    Done();
  }
  void setRaytracingPipelineLayout(const plume::RenderPipelineLayout *) override { Unsupported("raytracing"); }
  void setRaytracingPushConstants(uint32_t, const void *, uint32_t, uint32_t) override { Unsupported("raytracing"); }
  void setRaytracingDescriptorSet(plume::RenderDescriptorSet *, uint32_t) override { Unsupported("raytracing"); }
  void setIndexBuffer(const plume::RenderIndexBufferView *view) override {
    Put(Emit(kSetIndexBuffer, Size<plume::RenderIndexBufferView>()), *view);
    Done();
  }
  void setVertexBuffers(uint32_t start, const plume::RenderVertexBufferView *views, uint32_t count,
                        const plume::RenderInputSlot *slots) override {
    uint8_t *p = Emit(kSetVertexBuffers, Size<VbHead>() + Size<plume::RenderVertexBufferView>(count) +
                                             (slots ? Size<plume::RenderInputSlot>(count) : 0));
    p = Put(p, VbHead{start, count, slots ? 1u : 0u});
    p = PutArray(p, views, count);
    if (slots)
      PutArray(p, slots, count);
    Done();
  }
  void setViewports(const plume::RenderViewport *v, uint32_t count) override {
    uint8_t *p = Emit(kSetViewports, Size<CountHead>() + Size<plume::RenderViewport>(count));
    PutArray(Put(p, CountHead{count}), v, count);
    Done();
  }
  void setScissors(const plume::RenderRect *r, uint32_t count) override {
    uint8_t *p = Emit(kSetScissors, Size<CountHead>() + Size<plume::RenderRect>(count));
    PutArray(Put(p, CountHead{count}), r, count);
    Done();
  }
  void setFramebuffer(const plume::RenderFramebuffer *fb) override { PutPtr(kSetFramebuffer, fb); }
  void setDepthBias(float a, float b, float c) override {
    Put(Emit(kSetDepthBias, Size<DepthBias>()), DepthBias{a, b, c});
    Done();
  }
  void setStencilReference(uint32_t reference) override {
    Put(Emit(kSetStencilReference, Size<CountHead>()), CountHead{reference});
    Done();
  }
  void clearColor(uint32_t attachment, plume::RenderColor color, const plume::RenderRect *rects,
                  uint32_t count) override {
    uint8_t *p = Emit(kClearColor, Size<ClearColorHead>() + Size<plume::RenderRect>(count));
    PutArray(Put(p, ClearColorHead{attachment, color, count}), rects, count);
    Done();
  }
  void clearDepthStencil(bool depth, bool stencil, float value, uint32_t stencil_value,
                         const plume::RenderRect *rects, uint32_t count) override {
    uint8_t *p = Emit(kClearDepthStencil, Size<ClearDepthHead>() + Size<plume::RenderRect>(count));
    PutArray(Put(p, ClearDepthHead{depth ? 1u : 0u, stencil ? 1u : 0u, value, stencil_value, count}), rects, count);
    Done();
  }
  void copyBufferRegion(plume::RenderBufferReference dst, plume::RenderBufferReference src, uint64_t size) override {
    Put(Emit(kCopyBufferRegion, Size<CopyBufferRegion>()), CopyBufferRegion{dst, src, size});
    Done();
  }
  void copyTextureRegion(const plume::RenderTextureCopyLocation &dst, const plume::RenderTextureCopyLocation &src,
                         uint32_t x, uint32_t y, uint32_t z, const plume::RenderBox *box) override {
    uint8_t *p = Emit(kCopyTextureRegion, Size<CopyTextureRegionHead>() + (box ? Size<plume::RenderBox>() : 0));
    p = Put(p, CopyTextureRegionHead{dst, src, x, y, z, box ? 1u : 0u});
    if (box)
      Put(p, *box);
    Done();
  }
  void copyBuffer(const plume::RenderBuffer *dst, const plume::RenderBuffer *src) override {
    Put(Emit(kCopyBuffer, Size<TwoPtr>()), TwoPtr{dst, src});
    Done();
  }
  void copyTexture(const plume::RenderTexture *dst, const plume::RenderTexture *src) override {
    Put(Emit(kCopyTexture, Size<TwoPtr>()), TwoPtr{dst, src});
    Done();
  }
  void resolveTexture(const plume::RenderTexture *dst, const plume::RenderTexture *src) override {
    Put(Emit(kResolveTexture, Size<TwoPtr>()), TwoPtr{dst, src});
    Done();
  }
  void resolveTextureRegion(const plume::RenderTexture *dst, uint32_t x, uint32_t y, const plume::RenderTexture *src,
                            const plume::RenderRect *rect, plume::RenderResolveMode mode) override {
    uint8_t *p = Emit(kResolveTextureRegion, Size<ResolveRegionHead>() + (rect ? Size<plume::RenderRect>() : 0));
    p = Put(p, ResolveRegionHead{dst, x, y, src, rect ? 1u : 0u, mode});
    if (rect)
      Put(p, *rect);
    Done();
  }
  void buildBottomLevelAS(const plume::RenderAccelerationStructure *, plume::RenderBufferReference,
                          const plume::RenderBottomLevelASBuildInfo &) override {
    Unsupported("acceleration structures");
  }
  void buildTopLevelAS(const plume::RenderAccelerationStructure *, plume::RenderBufferReference,
                       plume::RenderBufferReference, const plume::RenderTopLevelASBuildInfo &) override {
    Unsupported("acceleration structures");
  }
  void discardTexture(const plume::RenderTexture *texture) override { PutPtr(kDiscardTexture, texture); }
  void resetQueryPool(const plume::RenderQueryPool *pool, uint32_t first, uint32_t count) override {
    Put(Emit(kResetQueryPool, Size<QueryHead>()), QueryHead{pool, first, count});
    Done();
  }
  void writeTimestamp(const plume::RenderQueryPool *pool, uint32_t index) override {
    Put(Emit(kWriteTimestamp, Size<QueryHead>()), QueryHead{pool, index, 0});
    Done();
  }
  void BlendConstants(const float rgba[4]) {
    Floats4 f;
    std::memcpy(f.v, rgba, sizeof(f.v));
    Put(Emit(kBlendConstants, Size<Floats4>()), f);
    Done();
  }

  // banjo-nx (2026-11-21, the deko3d command stage): banjo_renut_record.h's wrappers.
  uint32_t publish_commands_ = kPublishCommands;
  uint64_t buffer_waits_ = 0;
  void D3PushUniform(uint64_t address, uint32_t ubo_size, uint32_t offset, const void *data, uint32_t size) {
    uint8_t *p = Emit(kD3PushUniform, Size<PushUniformHead>() + Size<uint8_t>(size));
    p = Put(p, PushUniformHead{address, ubo_size, offset, size, 0});
    CopyPayload(p, static_cast<const uint8_t *>(data), size);
    Done();
  }
  // banjo-nx (2026-11-21, batch 2's ARM audit): the renderer's pushes are 16-byte multiples of at most a few hundred
  // bytes (shared constant lines, literal runs): copied inline, not by a library call each.
  static void CopyPayload(uint8_t *to, const uint8_t *from, uint32_t size) {
#if defined(__aarch64__)
    if ((size & 15u) == 0 && size <= 1024u) {
      for (uint32_t at = 0; at < size; at += 16)
        vst1q_u8(to + at, vld1q_u8(from + at));
      return;
    }
#endif
    if (size)
      std::memcpy(to, from, size);
  }
  void D3BindBuf(Op op, uint32_t slot, uint64_t address, uint32_t size) {
    Put(Emit(op, Size<BindBufHead>()), BindBufHead{address, slot, size});
    Done();
  }
  const uint8_t *shared_cpu_ = nullptr;  // the render thread's copy (Deko3DSetSharedWords), for the verify form
  uint32_t shared_size_ = 0;
  bool verify_ = false;
  void VerifyShared() {
    if (verify_ && shared_cpu_)
      Put(Emit(kD3VerifyShared, Size<VerifyHead>()), VerifyHead{SharedHash(shared_cpu_, shared_size_), shared_size_, 0});
  }
  void D3SetSharedWords(const void *cpu, uint32_t size) {
    shared_cpu_ = static_cast<const uint8_t *>(cpu);
    shared_size_ = cpu ? size : 0u;
    const uint32_t n = cpu ? size : 0u;
    uint8_t *p = Emit(kD3SetSharedWords, Size<SharedWordsHead>() + Size<uint8_t>(n));
    PutArray(Put(p, SharedWordsHead{n, cpu ? 1u : 0u}), static_cast<const uint8_t *>(cpu), n);
    Done();
  }
  void D3Word(Op op, uint32_t v) {
    Put(Emit(op, Size<CountHead>()), CountHead{v});
    Done();
  }
  void D3Bare(Op op) {
    Emit(op, 0);
    Done();
  }
  void D3FragTextures(uint32_t first, const uint32_t *slots, uint32_t count) {
    uint8_t *p = Emit(kD3BindFragTextures, Size<FragTexHead>() + Size<uint32_t>(count));
    PutArray(Put(p, FragTexHead{first, count}), slots, count);
    Done();
  }
  void D3Ptr(Op op, const void *ptr) { PutPtr(op, ptr); }
  void D3FetchData(uint64_t address, uint32_t size) {
    Put(Emit(kD3BindFetchData, Size<FetchHead>()), FetchHead{address, size, 0});
    Done();
  }
  void D3BufferWait(uint32_t invalidate, bool counts) {
    Put(Emit(kD3BufferWait, Size<BufferWaitHead>()), BufferWaitHead{invalidate, counts ? 1u : 0u});
    if (counts)
      ++buffer_waits_;
    Done();
  }
  void D3ReportCounter(const void *pool, uint32_t index, uint32_t counter) {
    Put(Emit(kD3ReportCounter, Size<ReportHead>()), ReportHead{pool, index, counter});
    Done();
  }
#if defined(PLUME_DEKO3D)
  void D3ImageTrack(const plume::Deko3DImageTrack &track) {
    Put(Emit(kD3ImageTrack, Size<plume::Deko3DImageTrack>()), track);
    Done();
  }
#endif
  bool Pending() const { return cur_ && wp_ != cur_->bytes.get(); }
  uint64_t calls_ = 0;  // StageCall's serials (render thread)
  uint64_t Call(void (*fn)(const void *), const void *args, uint32_t size) {
    const uint64_t serial = ++calls_;
    uint8_t *p = Emit(kStageCall, Size<CallHead>() + Size<uint8_t>(size));
    PutArray(Put(p, CallHead{fn, serial, size, 0}), static_cast<const uint8_t *>(args), size);
    Done();
    return serial;
  }
  void HandNow() { Publish(true); }
  uint8_t *EmitPublic(Op op, size_t payload) { return Emit(op, payload); }  // (item 3: the caller writes the payload)
  void DonePublic() { Done(); }

private:
  Chunk *cur_ = nullptr;
  uint32_t commands_ = 0;
  // banjo-nx (2026-11-21, batch 3): see Emit.
  uint8_t *wp_ = nullptr;
  uint8_t *end_ = nullptr;
  uint8_t *publish_at_ = nullptr;
  bool due_ = false;

  void PutPtr(Op op, const void *ptr) {
    Put(Emit(op, Size<Ptr>()), Ptr{ptr});
    Done();
  }
  void Push(Op op, uint32_t range, const void *data, uint32_t offset, uint32_t size) {
    if (size == 0) {
      // plume reads the whole range then, whose size the proxy does not know: every caller gives it (package 28b).
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXLOG_ERROR("[renut] record worker: push constants without a size (range {}); not recorded", range);
      }
      return;
    }
    uint8_t *p = Emit(op, Size<PushHead>() + Size<uint8_t>(size));
    PutArray(Put(p, PushHead{range, offset, size}), static_cast<const uint8_t *>(data), size);
    Done();
  }
  static void Unsupported(const char *what) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_ERROR("[renut] record worker: {} is not recorded (Renut's renderer does not use it)", what);
    }
  }
};

Recorder g_recorder;
// banjo-nx (2026-11-21, the deko3d command stage): a list is open on the proxy (between the Wrap that gave it and the
// next Wrap); the image tables' updates then go into the stream (render thread only).
bool g_stage_list = false;
std::atomic<uint64_t> g_image_tracks{0}, g_image_tracks_direct{0}, g_image_tracks_other_thread{0};

#if defined(PLUME_DEKO3D)
std::atomic<uint64_t> g_destroy_drains{0};
// banjo-nx (2026-11-21, batch 2): RetiredFreeScope's flag and the destructors it let go without a drain.
bool g_retired_free = false;
std::atomic<uint64_t> g_retired_free_skips{0};
// plume's object destructors (g_deko3dBeforeDestroyHook): on the render thread with a stage list open and anything
// recorded not played yet, the stage plays it all first (a command naming the object may be among it). The objects
// retired with the frame go when the stream is empty (the frame's drain came first): a test, no wait.
void BeforeDestroyHook() {
  if (!g_stage_list || !t_recording_thread)
    return;
  if (g_retired_free) {
    g_retired_free_skips.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  bool busy = g_recorder.Pending();
  if (!busy) {
    std::lock_guard lock(g_mutex);
    busy = g_played != g_published;
  }
  if (busy) {
    g_recorder.Drain();
    g_destroy_drains.fetch_add(1, std::memory_order_relaxed);
  }
}

// plume's setTexture, any thread: on the render thread with a stage list open, the image tables' update goes into the
// stream, after the draws recorded so far (the stage reads the tables in bindTextures and markSampled); else plume writes
// it now. (setTexture's callers hold State().mutex: the render thread's work, or none of it running.)
bool ImageTrackHook(const plume::Deko3DImageTrack &track) {
  if (!g_stage_list || !t_recording_thread) {
    if (g_stage_list)
      g_image_tracks_other_thread.fetch_add(1, std::memory_order_relaxed);
    else
      g_image_tracks_direct.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  g_recorder.D3ImageTrack(track);
  g_image_tracks.fetch_add(1, std::memory_order_relaxed);
  return true;
}
#endif

}  // namespace

#if defined(PLUME_DEKO3D)
plume::RenderCommandList *g_proxy = nullptr;
#endif

plume::RenderCommandList *Wrap(plume::RenderCommandList *real) {
  t_recording_thread = true;
  // Vulkan: the blend constant is played as a Vulkan command (SetBlendConstant's own call). deko3d: the command stage.
  const bool deko3d = ::renut::gpu::IsDeko3D();
  const bool use = real && (deko3d ? StageOn() : (::renut::gpu::IsVulkan() && On()));
  if (!use) {
    // banjo-nx (2026-11-21): from the stage to direct recording (an A/B half): what the stream still holds (image tables'
    // updates made since the last drain) is played first, so they stay in order with what the render thread does now.
    if (g_stage_list) {
      g_stage_list = false;
      if (g_recorder.Pending() || g_started)
        g_recorder.Drain();
    }
    g_direct_lists.fetch_add(1, std::memory_order_relaxed);
    return real;
  }
  {
    std::lock_guard lock(g_mutex);
    if (!g_started) {
      g_started = true;
#if defined(PLUME_DEKO3D)
      if (deko3d) {
        g_deko3d_stage = true;  // before the worker starts (it reads it once)
        g_recorder.publish_commands_ = uint32_t(REXCVAR_GET(banjo_deko3d_command_stage_batch));
        g_recorder.verify_ = REXCVAR_GET(banjo_deko3d_command_stage_verify);
        g_proxy = &g_recorder;
        plume::Deko3DSetImageTrackHook(&ImageTrackHook);
        plume::g_deko3dBeforeDestroyHook = &BeforeDestroyHook;
      }
#endif
      StartLongLivedThread(&Worker, 256u << 10);
    }
  }
  // Every earlier list was drained before its submit: the stream holds no command for an earlier list here (at most
  // image tables' updates, which name no list), so the target switches cleanly.
  g_recorder.real = real;
  g_stage_list = deko3d;
  g_lists.fetch_add(1, std::memory_order_relaxed);
  return &g_recorder;
}

#if defined(PLUME_DEKO3D)
namespace detail {
void PushUniform(uint64_t address, uint32_t uboSize, uint32_t offset, const void *data, uint32_t size) {
  g_recorder.D3PushUniform(address, uboSize, offset, data, size);
}
void BindUniform(uint32_t slot, uint64_t address, uint32_t size) { g_recorder.D3BindBuf(kD3BindUniform, slot, address, size); }
void BindStorage(uint32_t slot, uint64_t address, uint32_t size) { g_recorder.D3BindBuf(kD3BindStorage, slot, address, size); }
void SetSharedWords(const void *cpu, uint32_t size) { g_recorder.D3SetSharedWords(cpu, size); }
void SharedLinesWritten(uint32_t lines) { g_recorder.D3Word(kD3SharedLines, lines); }
void BindFragmentTextures(uint32_t first, const uint32_t *slots, uint32_t count) {
  g_recorder.D3FragTextures(first, slots, count);
}
void BindMemExport(const plume::RenderBuffer *buffer) { g_recorder.D3Ptr(kD3BindMemExport, buffer); }
void BindFetchData(uint64_t address, uint32_t size) { g_recorder.D3FetchData(address, size); }
void FlushL2() { g_recorder.D3Bare(kD3FlushL2); }
void DiscardNextDraw() { g_recorder.D3Bare(kD3DiscardNextDraw); }
void SetSampleMask(uint32_t mask) { g_recorder.D3Word(kD3SampleMask, mask); }
void SetNextPrimitive(uint32_t primitive) { g_recorder.D3Word(kD3NextPrimitive, primitive); }
void BufferWait(uint32_t invalidate, bool countsAsBufferWait) { g_recorder.D3BufferWait(invalidate, countsAsBufferWait); }
uint64_t BufferWaits() { return g_recorder.buffer_waits_; }
void ReportCounter(const plume::RenderQueryPool *pool, uint32_t index, uint32_t counter) {
  g_recorder.D3ReportCounter(pool, index, counter);
}
uint8_t *PushUniformRawBegin(uint64_t address, uint32_t uboSize, uint32_t offset, uint32_t size, bool nanFlush) {
  uint8_t *p = g_recorder.EmitPublic(kD3PushUniformRaw, (sizeof(PushUniformHead) + 7) / 8 * 8 + (size + 7u) / 8 * 8);
  const PushUniformHead head{address, uboSize, offset, size, nanFlush ? 1u : 0u};
  std::memcpy(p, &head, sizeof(head));
  return p + (sizeof(PushUniformHead) + 7) / 8 * 8;
}
void PushUniformRawEnd() { g_recorder.DonePublic(); }
}  // namespace detail
#endif

void Drain() {
  if (g_started)
    g_recorder.Drain();
}

uint64_t StageCall(void (*fn)(const void *args), const void *args, uint32_t size) {
  return g_recorder.Call(fn, args, size);
}

void Hand() {
  if (g_started && g_recorder.Pending())
    g_recorder.HandNow();
}

void WaitStageCall(uint64_t serial) {
  if (!serial || g_calls_done.load(std::memory_order_acquire) >= serial)
    return;
  if (g_recorder.Pending())
    g_recorder.HandNow();  // (the call may be in the chunk still being recorded)
  const auto t0 = std::chrono::steady_clock::now();
  {
    std::unique_lock lock(g_mutex);
    ::renut::gpu::render_thread::WaitScope wait;
    ++g_done_waiters;
    g_done.wait(lock, [serial] { return g_calls_done.load(std::memory_order_acquire) >= serial; });
    --g_done_waiters;
  }
  g_drain_wait_us.fetch_add(
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count()),
      std::memory_order_relaxed);
}

// banjo-nx (2026-11-21, batch 2): banjo_deko3d_stage_retired_free.
RetiredFreeScope::RetiredFreeScope(bool played) {
#if defined(PLUME_DEKO3D)
  static const bool key = REXCVAR_GET(banjo_deko3d_stage_retired_free);
  on = played && key && !g_retired_free;
  if (on)
    g_retired_free = true;
#else
  (void)played;
  on = false;
#endif
}
RetiredFreeScope::~RetiredFreeScope() {
#if defined(PLUME_DEKO3D)
  if (on)
    g_retired_free = false;
#endif
}

bool SetBlendConstants(plume::RenderCommandList *cmd, const float rgba[4]) {
  if (cmd != &g_recorder)
    return false;
  g_recorder.BlendConstants(rgba);
  return true;
}

bool IsProxy(const plume::RenderCommandList *cmd) {
  return cmd == &g_recorder;
}

// banjo-nx (2026-11-22, banjo_deko3d_command_stage_float, A/B 189): the calling thread (the stage's, the warm helper's)
// on cores 1-3 with core 3 preferred, or on core 3 alone, as the key and the A/B's half say; the system call only when
// that changes (its 'applied' is the thread's own: 0 unset, 1 core 3, 2 cores 1-3).
void FollowStageCores(int &applied) {
#if REX_PLATFORM_SWITCH
  static const bool key = REXCVAR_GET(banjo_deko3d_command_stage_float);
  const int want = key && g_ab_off.load(std::memory_order_relaxed) != 189 ? 2 : 1;
  if (want == applied)
    return;
  applied = want;
  const int sys = RexSwitchSystemCore();
  if (sys < 0)
    return;
  if (want == 2)
    (void)RexSwitchSetCurrentThreadCoresWithSystem(sys, 0xEu);
  else
    (void)RexSwitchMoveCurrentThreadToSystemCore(-1);
#else
  (void)applied;
#endif
}

bool StageActive() { return StageOn(); }

void Shutdown() {
  if (!g_started)
    return;
  g_recorder.Drain();
  std::unique_lock lock(g_mutex);
  g_stop = true;
  g_work.notify_all();
  ++g_done_waiters;
  g_done.wait(lock, [] { return g_exited; });
  --g_done_waiters;
}

void Report(int seconds) {
  (void)seconds;
  const uint64_t lists = g_lists.exchange(0, std::memory_order_relaxed);
  const uint64_t direct = g_direct_lists.exchange(0, std::memory_order_relaxed);
  if (!lists && !g_started)
    return;
  const uint64_t commands = g_commands.exchange(0, std::memory_order_relaxed);
  const uint64_t chunks = g_chunks.exchange(0, std::memory_order_relaxed);
  const uint64_t drains = g_drains.exchange(0, std::memory_order_relaxed);
  const uint64_t wait_us = g_drain_wait_us.exchange(0, std::memory_order_relaxed);
  const uint64_t busy_us = g_worker_busy_us.exchange(0, std::memory_order_relaxed);
  const uint64_t wakes = g_wakes.exchange(0, std::memory_order_relaxed);
#if defined(PLUME_DEKO3D)
  const uint64_t destroy_drains = g_destroy_drains.exchange(0, std::memory_order_relaxed);
  const uint64_t retired_skips = g_retired_free_skips.exchange(0, std::memory_order_relaxed);
#else
  const uint64_t destroy_drains = 0;
  const uint64_t retired_skips = 0;
#endif
  REXLOG_INFO("[renut] record worker (banjo_renut_record_worker{}): {} lists through it, {} direct; {} commands in {} "
              "chunks ({} wakes); worker playing {:.1f} ms; the render thread waited {:.1f} ms in {} drains; image "
              "tables' updates {} in the stream, {} direct, {} off the render thread with a stage list open; drains "
              "before an object went {} ({} retired objects went without one); shared copies checked {}, differ {}",
              g_deko3d_stage ? ", banjo_deko3d_command_stage" : "", lists, direct, commands, chunks, wakes,
              double(busy_us) / 1000.0, double(wait_us) / 1000.0, drains,
              g_image_tracks.exchange(0, std::memory_order_relaxed),
              g_image_tracks_direct.exchange(0, std::memory_order_relaxed),
              g_image_tracks_other_thread.exchange(0, std::memory_order_relaxed), destroy_drains, retired_skips,
              g_shared_checked.exchange(0, std::memory_order_relaxed), g_shared_differ.load(std::memory_order_relaxed));
}

}  // namespace banjo::renut_gpu::record
