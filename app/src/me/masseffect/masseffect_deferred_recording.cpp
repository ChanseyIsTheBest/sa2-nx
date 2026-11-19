// Deferred Vulkan command recording: see masseffect_deferred_recording.h.
#include "masseffect_deferred_recording.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>
#include <unordered_map>

#include <rex/platform.h>
#if REX_PLATFORM_SWITCH
#include <switch.h>
#else
#include <thread>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../me_platform_compat.h"  // banjo-nx: banjo_deferred_core (RexSwitchPinCurrentThreadToCore)

REXCVAR_DEFINE_BOOL(masseffect_native_deferred_recording, false, "Mass Effect",
                    "Ring thread queues every vkCmd* call and a worker thread records them into the same command "
                    "buffers in the same order (moves NVK recording off the ring thread; same commands, same image)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_deferred_recording_parts, 3, "Mass Effect",
                     "Diagnostic bisection of masseffect_native_deferred_recording: 1 = proxy function table, "
                     "2 = wrappers for the functions fetched by name")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_deferred_recording_naps, 64, "Mass Effect",
                     "Worker: 5 us naps on an empty queue before it blocks on the condition variable "
                     "(each nap preempts a game thread on its core)")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// banjo-nx (2026-10-16): per-draw packets. The PC in-run A/B of package 21b's tree (target 16, console settings) had
// the ring at 9.86 us a draw with the layer on against 4.96 off: queueing each of the ~15 calls of a draw (a slot, a
// tuple, an arena copy and a publish test each) cost more than the calls it moved. The draw path's own commands
// (pipeline, descriptor sets, push constants, dynamic state, vertex and index buffers, the draw) are now appended as
// compact records to one open chunk of the arena; the chunk becomes ONE queue slot when anything else is queued,
// when it is full, or when the ring publishes. Same commands, same order.
REXCVAR_DEFINE_BOOL(banjo_deferred_packets, true, "Banjo",
                    "masseffect_native_deferred_recording: the draw path's per-draw commands are queued as packed "
                    "records in one slot (many draws per slot) instead of one slot per Vulkan call. Same commands in "
                    "the same order; false = one slot per call, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-16): the worker was created on the process's ideal core with that core alone as its mask (libnx
// threadCreate with -2), core 0, where the main (UI) thread is pinned: the console test of 2026-10-14 ("slightly
// worse") had the worker and the UI thread sharing one core while the ring floated. With banjo_ring_core on core 1,
// the worker is pinned to its own core here.
REXCVAR_DEFINE_INT32(banjo_deferred_core, 2, "Banjo",
                     "masseffect_native_deferred_recording: the worker thread is pinned to this core (-1 = left "
                     "where it was created)")
    .range(-1, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_deferred_native_fast, false, "Mass Effect",
                    "Queue: the producer publishes commands in batches (masseffect_deferred_native_batch) instead "
                    "of one seq_cst store + wake check per command, keeps its own copy of the worker's read index "
                    "and the worker stores it every 16 commands (less cache line ping-pong between the cores). "
                    "Same commands, same order; false = as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_deferred_native_batch, 16, "Mass Effect",
                     "masseffect_deferred_native_fast: commands queued before the worker is told (the queue is also "
                     "published before the ring thread waits, drains or idles)")
    .range(1, 256)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-15): what the renderer asks of the driver each frame, for the GPU's front end.
REXCVAR_DEFINE_BOOL(banjo_diag_vk_calls, false, "Banjo",
                    "Measurement: count every Vulkan command the renderer records (per Swap, in the 10 s report), "
                    "with its barriers (image, buffer and memory barriers, layout changes, barriers whose access "
                    "masks include MEMORY_READ / MEMORY_WRITE: on NVK a full cache flush, ALL_COMMANDS stages) and "
                    "render passes by their depth and colour load and store operations. Changes no command")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_deferred_native_update, false, "Mass Effect",
                    "vkUpdateDescriptorSets is queued too (its write arrays deep-copied into the arena) instead of "
                    "draining the worker (~70 drains per frame, each a spin-yield of the ring thread). The update runs "
                    "on the worker at the same position of the command stream; every submit, destroy and reset still "
                    "drains first")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace masseffect::native::deferred {
namespace {

// ---------------------------------------------------------------------------------------------------------------
// Queue: single producer (the ring thread), single consumer (the worker).
constexpr size_t kSlots = size_t(1) << 10;  // 2 MB: the process runs within a few MB of its limit
constexpr size_t kLoad = 112;
struct alignas(16) Slot {
  void (*run)(void*) = nullptr;
  alignas(16) unsigned char load[kLoad];
};
Slot* g_queue = nullptr;
alignas(64) std::atomic<uint64_t> g_written{0};
alignas(64) std::atomic<uint64_t> g_read{0};
uint64_t g_written_producer = 0;
// masseffect_deferred_native_fast: what the worker was last told (g_written), the producer's copy of the
// worker's read index (it only reloads the shared one when the queue looks full) and the batch size.
uint64_t g_published = 0;
uint64_t g_read_view = 0;
bool g_fast = false;
uint32_t g_batch = 1;
uint64_t g_batches = 0, g_updates = 0, g_direct_updates = 0;
std::atomic<bool> g_sleeping{false};
std::mutex g_mutex;
std::condition_variable g_cv;

std::atomic<bool> g_active{false};
std::atomic<bool> g_pinned_producer{false};
thread_local bool t_producer = false;  // not std::thread::id: SDK threads are not pthreads

// Arrays of the queued calls (barriers, regions, viewports...). Reset whenever the queue is empty.
constexpr size_t kArena = size_t(1) << 20;  // banjo-nx: 1 MB (was 512 KB): the draw packets live here too
uint8_t* g_arena = nullptr;
size_t g_arena_usage = 0;

// Report.
uint64_t g_queued = 0, g_direct = 0, g_drains = 0, g_drains_with_wait = 0, g_ns_wait = 0;
uint64_t g_packet_ops = 0, g_packet_slots = 0;  // banjo_deferred_packets (report)
// banjo-nx: what caused each drain (the call that needed the queue empty), for the 10 s report. Producer thread only.
struct DrainReason {
  const char* name = nullptr;
  uint64_t drains = 0, waits = 0, ns = 0;
};
DrainReason g_reasons[48];
const char* g_reason = "other";
auto g_report = std::chrono::steady_clock::now();

inline bool IsProducer() {
  return t_producer && g_active.load(std::memory_order_relaxed);
}

void Wake() {
  if (g_sleeping.load(std::memory_order_seq_cst)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cv.notify_one();
  }
}

// Tells the worker about everything queued so far (the batched mode may hold a few commands back).
void PublishAlready() {
  g_published = g_written_producer;
  ++g_batches;
  g_written.store(g_written_producer, std::memory_order_seq_cst);
  Wake();
}

Slot& Reserve() {
  if (g_fast) {
    if (g_written_producer - g_read_view >= kSlots) {
      if (g_published != g_written_producer) PublishAlready();  // the worker cannot free slots it was not told about
      for (;;) {
        g_read_view = g_read.load(std::memory_order_acquire);
        if (g_written_producer - g_read_view < kSlots) break;
        Wake();
        std::this_thread::sleep_for(std::chrono::microseconds(20));
      }
    }
    return g_queue[g_written_producer & (kSlots - 1)];
  }
  while (g_written_producer - g_read.load(std::memory_order_acquire) >= kSlots) {
    Wake();
    std::this_thread::sleep_for(std::chrono::microseconds(20));
  }
  return g_queue[g_written_producer & (kSlots - 1)];
}

void Publish() {
  ++g_written_producer;
  ++g_queued;
  if (g_fast) {
    if (g_written_producer - g_published >= g_batch) PublishAlready();
    return;
  }
  g_written.store(g_written_producer, std::memory_order_seq_cst);
  Wake();
}

int32_t g_priority_producer = -1;

void Worker() {
  uint64_t read = 0;
  uint32_t empty = 0;
  const bool fast = g_fast;
  {  // banjo_deferred_core
    const int32_t core = REXCVAR_GET(banjo_deferred_core);
    const int before = RexSwitchCurrentCore();
    const bool ok = core >= 0 && RexSwitchPinCurrentThreadToCore(int(core));
    REXLOG_INFO("[native] deferred recording worker: {} (was on core {}, now on core {})",
                core < 0 ? std::string("not pinned (banjo_deferred_core = -1)")
                         : fmt::format("pinned to core {}{}", core, ok ? "" : " (NOT accepted by the kernel)"),
                before, RexSwitchCurrentCore());
  }
  for (;;) {
    const uint64_t written = g_written.load(std::memory_order_acquire);
    if (read < written) {
      if (fast) {
        // The producer only needs the read index when the queue is full or to drain: every 16 commands and at the
        // end of the burst (drain waits for g_read == g_written_producer, which the final store provides).
        do {
          Slot& r = g_queue[read & (kSlots - 1)];
          r.run(r.load);
          ++read;
          if ((read & 15) == 0) g_read.store(read, std::memory_order_release);
        } while (read < written);
        g_read.store(read, std::memory_order_release);
      } else {
        do {
          Slot& r = g_queue[read & (kSlots - 1)];
          r.run(r.load);
          ++read;
          g_read.store(read, std::memory_order_release);
        } while (read < written);
      }
      empty = 0;
      continue;
    }
    // A short spin (the ring publishes in bursts), then sleep: the three cores are shared with the game.
    static const uint32_t naps = uint32_t(REXCVAR_GET(masseffect_native_deferred_recording_naps));
    if (++empty < naps) {
      std::this_thread::sleep_for(std::chrono::microseconds(5));
      continue;
    }
    std::unique_lock<std::mutex> lock(g_mutex);
    g_sleeping.store(true, std::memory_order_seq_cst);
    if (g_written.load(std::memory_order_seq_cst) == read) {
      g_cv.wait_for(lock, std::chrono::milliseconds(2));
    }
    g_sleeping.store(false, std::memory_order_seq_cst);
    empty = 0;
  }
}

void ClosePacket();

void DrainInternal() {
  ClosePacket();  // banjo_deferred_packets: the open chunk is queued work too
  const char* const reason = g_reason;
  g_reason = "other";
  uint64_t waited_ns = 0;
  bool waited = false;
  static uint32_t traces = 0;
  if (traces < 6) {
    ++traces;
    REXLOG_INFO("[native] deferred recording: drain {}: written {} read {}", traces, g_written_producer,
                g_read.load(std::memory_order_acquire));
  }
  if (g_fast && g_published != g_written_producer) PublishAlready();
  if (g_read.load(std::memory_order_acquire) != g_written_producer) {
    const auto t0 = std::chrono::steady_clock::now();
    Wake();
    // Not yield(): on Horizon it only gives way to threads of the SAME priority on the SAME core, so a worker
    // on this core with a lower priority would never run (the first builds of this worker hung exactly like that).
    for (uint32_t laps = 0; g_read.load(std::memory_order_acquire) != g_written_producer; ++laps) {
      if (laps < 32) std::this_thread::yield();
      else std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
    waited_ns = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    g_ns_wait += waited_ns;
    waited = true;
    ++g_drains_with_wait;
  }
  ++g_drains;
  for (DrainReason& r : g_reasons) {  // by pointer: the names are string literals
    if (r.name && r.name != reason) continue;
    r.name = reason;
    ++r.drains;
    r.waits += waited ? 1 : 0;
    r.ns += waited_ns;
    break;
  }
  g_read_view = g_written_producer;  // the queue is empty (the worker has executed everything)
  g_arena_usage = 0;
  const auto now = std::chrono::steady_clock::now();
  if (now - g_report >= std::chrono::seconds(10)) {
    g_report = now;
    REXLOG_INFO("[native] deferred recording (10 s): {} calls queued, {} direct on the ring thread; {} "
                "drains ({} waiting for the worker, {:.1f} ms in total); {} publishes; vkUpdateDescriptorSets "
                "{} queued, {} direct; draw packets (banjo_deferred_packets): {} records in {} slots",
                g_queued, g_direct, g_drains, g_drains_with_wait, double(g_ns_wait) / 1e6, g_batches,
                g_updates, g_direct_updates, g_packet_ops, g_packet_slots);
    g_queued = g_direct = g_drains = g_drains_with_wait = g_ns_wait = 0;
    g_packet_ops = g_packet_slots = 0;
    g_batches = g_updates = g_direct_updates = 0;
    std::vector<DrainReason> reasons;
    for (DrainReason& r : g_reasons)
      if (r.name && r.drains) reasons.push_back(r);
    std::sort(reasons.begin(), reasons.end(),
              [](const DrainReason& a, const DrainReason& b) { return a.ns != b.ns ? a.ns > b.ns : a.drains > b.drains; });
    std::string text;
    for (size_t i = 0; i < reasons.size() && i < 10; ++i)
      text += fmt::format(" | {}: {} drains, {} waiting, {:.1f} ms", reasons[i].name, reasons[i].drains,
                          reasons[i].waits, double(reasons[i].ns) / 1e6);
    if (!text.empty()) REXLOG_INFO("[native] deferred recording drains by cause (10 s){}", text);
    for (DrainReason& r : g_reasons) r.drains = r.waits = r.ns = 0;
  }
}

// One arena block for all arrays of one call (a drain inside would reuse memory already copied for it).
uint8_t* ArenaRaw(size_t bytes);
uint8_t* Arena(size_t bytes) {
  ClosePacket();  // banjo_deferred_packets: nothing may be allocated after an open chunk
  return ArenaRaw(bytes);
}
uint8_t* ArenaRaw(size_t bytes) {
  bytes = (bytes + 15) & ~size_t(15);
  if (bytes > kArena) return nullptr;
  if (g_arena_usage + bytes > kArena) {
    g_reason = "arena full";
    DrainInternal();
  }
  uint8_t* p = g_arena + g_arena_usage;
  g_arena_usage += bytes;
  return p;
}
template <typename T>
size_t Size(uint32_t n) {
  return (sizeof(T) * n + 15) & ~size_t(15);
}
template <typename T>
const T* Copy(uint8_t*& cursor, const T* src_data, uint32_t n) {
  if (!src_data || !n) return src_data;
  T* target = reinterpret_cast<T*>(cursor);
  std::memcpy(target, src_data, sizeof(T) * n);
  cursor += Size<T>(n);
  return target;
}
template <typename T>
bool WithPNext(const T* p, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i)
    if (p[i].pNext) return true;
  return false;
}

bool g_no_enqueue = false;  // bisection bit 32: producer and worker set up, calls made at once

template <typename F, typename... A>
void Enqueue(F f, A... a) {
  if (g_no_enqueue) {
    f(a...);
    return;
  }
  ClosePacket();  // banjo_deferred_packets: the draws appended so far go first
  struct Package {
    F f;
    std::tuple<A...> args;
  };
  static_assert(sizeof(Package) <= kLoad && alignof(Package) <= 16, "call too large for the queue");
  Slot& r = Reserve();
  new (r.load) Package{f, std::tuple<A...>(a...)};
  r.run = [](void* c) {
    Package* p = static_cast<Package*>(c);
    std::apply(p->f, p->args);
    p->~Package();
  };
  Publish();
}

// ---------------------------------------------------------------------------------------------------------------
// banjo-nx (2026-10-16): draw packets (banjo_deferred_packets). Records of the draw path's commands, 8-byte aligned,
// appended to one open chunk of the arena; ClosePacket turns the chunk into one queue slot. Anything else that is
// queued (Enqueue, Arena) or that empties the queue closes it first, so the commands keep their order.
Functions g_real;  // the driver's functions (the worker calls them; set when the table is built)
bool g_packets = false;
uint8_t* g_packet = nullptr;  // the open chunk, or null
uint8_t* g_packet_cur = nullptr;
uint8_t* g_packet_end = nullptr;
VkCommandBuffer g_packet_cb = VK_NULL_HANDLE;
constexpr size_t kPacketChunk = size_t(8) << 10;

enum PacketOp : uint32_t {
  kOpBindPipeline = 1,
  kOpBindSets,
  kOpPush,
  kOpViewport,
  kOpScissor,
  kOpBlend,
  kOpBias,
  kOpStencil,
  kOpVertexBuffers,
  kOpIndexBuffer,
  kOpDrawIndexed,
  kOpDraw,
};

struct PacketLoad {
  const uint8_t* data;
  size_t size;
  VkCommandBuffer cb;
};

template <typename T>
inline void Put(uint8_t*& p, const T& v) {
  std::memcpy(p, &v, sizeof(T));
  p += sizeof(T);
}
template <typename T>
inline T Get(const uint8_t*& p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  p += sizeof(T);
  return v;
}
inline size_t Up8(size_t n) { return (n + 7) & ~size_t(7); }

// Worker: the records of one chunk, in order.
void RunPacket(void* c) {
  const PacketLoad& load = *static_cast<const PacketLoad*>(c);
  const uint8_t* p = load.data;
  const uint8_t* const end = p + load.size;
  const VkCommandBuffer cb = load.cb;
  while (p < end) {
    const uint8_t* const start = p;
    const uint32_t op = Get<uint32_t>(p);
    const uint32_t bytes = Get<uint32_t>(p);  // the record's size, header included
    switch (op) {
      case kOpBindPipeline:
        g_real.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, Get<VkPipeline>(p));
        break;
      case kOpBindSets: {
        const VkPipelineLayout layout = Get<VkPipelineLayout>(p);
        const uint32_t first = Get<uint32_t>(p), n = Get<uint32_t>(p), nd = Get<uint32_t>(p);
        Get<uint32_t>(p);
        VkDescriptorSet sets[8];
        uint32_t offsets[8];
        for (uint32_t i = 0; i < n; ++i) sets[i] = Get<VkDescriptorSet>(p);
        for (uint32_t i = 0; i < nd; ++i) offsets[i] = Get<uint32_t>(p);
        g_real.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, first, n, sets, nd,
                                       nd ? offsets : nullptr);
        break;
      }
      case kOpPush: {
        const VkPipelineLayout layout = Get<VkPipelineLayout>(p);
        const uint32_t stages = Get<uint32_t>(p), offset = Get<uint32_t>(p), size = Get<uint32_t>(p);
        Get<uint32_t>(p);
        g_real.vkCmdPushConstants(cb, layout, stages, offset, size, p);
        break;
      }
      case kOpViewport: {
        const VkViewport v = Get<VkViewport>(p);
        g_real.vkCmdSetViewport(cb, 0, 1, &v);
        break;
      }
      case kOpScissor: {
        const VkRect2D r = Get<VkRect2D>(p);
        g_real.vkCmdSetScissor(cb, 0, 1, &r);
        break;
      }
      case kOpBlend: {
        float k[4];
        for (float& x : k) x = Get<float>(p);
        g_real.vkCmdSetBlendConstants(cb, k);
        break;
      }
      case kOpBias: {
        const float constant = Get<float>(p), clamp = Get<float>(p), slope = Get<float>(p);
        g_real.vkCmdSetDepthBias(cb, constant, clamp, slope);
        break;
      }
      case kOpStencil: {
        const uint32_t front = Get<uint32_t>(p), back = Get<uint32_t>(p);
        g_real.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, front & 0xFF);
        g_real.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, back & 0xFF);
        g_real.vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, (front >> 8) & 0xFF);
        g_real.vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, (back >> 8) & 0xFF);
        g_real.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, (front >> 16) & 0xFF);
        g_real.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, (back >> 16) & 0xFF);
        break;
      }
      case kOpVertexBuffers: {
        const uint32_t first = Get<uint32_t>(p), n = Get<uint32_t>(p);
        VkBuffer buffers[32];
        VkDeviceSize offsets[32];
        for (uint32_t i = 0; i < n; ++i) buffers[i] = Get<VkBuffer>(p);
        for (uint32_t i = 0; i < n; ++i) offsets[i] = Get<VkDeviceSize>(p);
        g_real.vkCmdBindVertexBuffers(cb, first, n, buffers, offsets);
        break;
      }
      case kOpIndexBuffer: {
        const VkIndexType type = VkIndexType(Get<uint32_t>(p));
        Get<uint32_t>(p);
        const VkBuffer buffer = Get<VkBuffer>(p);
        const VkDeviceSize offset = Get<VkDeviceSize>(p);
        g_real.vkCmdBindIndexBuffer(cb, buffer, offset, type);
        break;
      }
      case kOpDrawIndexed: {
        const uint32_t count = Get<uint32_t>(p), instances = Get<uint32_t>(p), first = Get<uint32_t>(p);
        const int32_t vertex_offset = Get<int32_t>(p);
        const uint32_t first_instance = Get<uint32_t>(p);
        g_real.vkCmdDrawIndexed(cb, count, instances, first, vertex_offset, first_instance);
        break;
      }
      case kOpDraw: {
        const uint32_t count = Get<uint32_t>(p), instances = Get<uint32_t>(p), first = Get<uint32_t>(p),
                       first_instance = Get<uint32_t>(p);
        g_real.vkCmdDraw(cb, count, instances, first, first_instance);
        break;
      }
      default:
        REXLOG_ERROR("[native] deferred recording: unknown packet record {} ({} bytes): rest of the chunk dropped",
                     op, bytes);
        return;
    }
    p = start + bytes;
  }
}

void ClosePacket() {
  if (!g_packet) return;
  const size_t used = size_t(g_packet_cur - g_packet);
  // The rest of the chunk goes back to the arena: nothing was allocated after it while it was open (Arena closes it).
  g_arena_usage -= kPacketChunk - ((used + 15) & ~size_t(15));
  const PacketLoad load{g_packet, used, g_packet_cb};
  g_packet = nullptr;
  if (!used) return;
  Slot& r = Reserve();
  static_assert(sizeof(PacketLoad) <= kLoad, "packet load too large for a slot");
  new (r.load) PacketLoad(load);
  r.run = &RunPacket;
  ++g_packet_slots;
  Publish();
}

// Room for one record of `bytes` (header included, a multiple of 8) in the open chunk for `cb`.
uint8_t* PacketSpace(VkCommandBuffer cb, size_t bytes) {
  if (g_packet && (cb != g_packet_cb || g_packet_cur + bytes > g_packet_end)) ClosePacket();
  if (!g_packet) {
    g_packet = ArenaRaw(kPacketChunk);
    g_packet_cur = g_packet;
    g_packet_end = g_packet + kPacketChunk;
    g_packet_cb = cb;
  }
  uint8_t* const p = g_packet_cur;
  g_packet_cur += bytes;
  ++g_packet_ops;
  return p;
}

inline bool PacketsTake() { return g_packets && !g_no_enqueue && IsProducer(); }

// ---------------------------------------------------------------------------------------------------------------
// Generic wrappers, one per function of the table (and per name fetched with Proc).
template <typename T>
constexpr bool kPointerData = std::is_pointer_v<T> && std::is_const_v<std::remove_pointer_t<T>>;

enum Mode : uint8_t { kDirect = 0, kDrain = 1, kDefer = 2 };

template <size_t Id, typename PFN>
struct Wrapper;
template <size_t Id, typename R, typename... A>
struct Wrapper<Id, R (*)(A...)> {
  using Pointer = R (*)(A...);
  static constexpr bool kDeferrable = std::is_void_v<R> && !(kPointerData<A> || ...);
  static inline Pointer real = nullptr;
  static inline uint8_t mode = kDirect;
  static inline const char* name = "?";
  static R F(A... a) {
    if (IsProducer()) {
      if constexpr (kDeferrable) {
        if (mode == kDefer) {
          Enqueue(real, a...);
          return;
        }
      }
      if (mode != kDirect) {
        g_reason = name;
        DrainInternal();
        ++g_direct;
      }
    }
    return real(a...);
  }
};

uint8_t Classify(std::string_view n, bool deferrable) {
  if (n.starts_with("vkCmd")) return deferrable ? kDefer : kDrain;
  static constexpr std::string_view kDrainNames[] = {
      "vkBeginCommandBuffer", "vkEndCommandBuffer", "vkResetCommandBuffer", "vkResetCommandPool",
      "vkAllocateCommandBuffers", "vkQueueSubmit", "vkQueueSubmit2", "vkQueueWaitIdle", "vkDeviceWaitIdle",
      "vkQueuePresentKHR", "vkUpdateDescriptorSets", "vkResetDescriptorPool"};
  for (auto k : kDrainNames)
    if (n == k) return kDrain;
  if (n.starts_with("vkDestroy") || n.starts_with("vkFree")) return kDrain;
  return kDirect;
}

template <size_t Id, typename PFN>
void Install(PFN real, PFN& target, const char* name) {
  using E = Wrapper<Id, PFN>;
  if (!real) return;
  E::real = real;
  E::name = name;
  E::mode = Classify(name, E::kDeferrable);
  if (E::mode != kDirect) target = &E::F;
}

Functions g_proxy;  // banjo-nx: g_real is defined above the draw packets, which call it

// ---------------------------------------------------------------------------------------------------------------
// Calls with arrays or structures: their data is copied into the arena.
VkResult BeginCommands(VkCommandBuffer cb, const VkCommandBufferBeginInfo* info) {
  if (g_active.load(std::memory_order_relaxed) && !g_pinned_producer.load(std::memory_order_relaxed)) {
    REXLOG_INFO("[native] deferred recording: first command buffer, starting the worker");
    bool created = false;
#if REX_PLATFORM_SWITCH
    {
      s32 priority = 0;
      if (R_SUCCEEDED(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE))) g_priority_producer = int32_t(priority);
    }
    // A libnx thread, not std::thread: the ring thread is a Horizon thread created by the SDK, and a pthread
    // created from it hung the whole process.
    static Thread thread;
    if (R_SUCCEEDED(threadCreate(&thread, [](void*) { Worker(); }, nullptr, nullptr, 256 * 1024,
                                 g_priority_producer >= 0 ? g_priority_producer : 0x2C, -2)) &&
        R_SUCCEEDED(threadStart(&thread))) {
      created = true;
    }
#else
    // banjo-nx: PC build, a plain thread (kept for the life of the process, like the Switch one).
    static std::thread thread([] { Worker(); });
    created = true;
#endif
    if (created) {
      t_producer = true;
      g_pinned_producer.store(true, std::memory_order_release);
      REXLOG_INFO("[native] deferred recording active: the thread that began the first command buffer queues, "
                  "a worker records");
    } else {
      g_active.store(false, std::memory_order_release);
      REXLOG_ERROR("[native] deferred recording DISABLED: could not create the worker");
    }
  }
  if (IsProducer()) {
    g_reason = "vkBeginCommandBuffer";
    DrainInternal();
  }
  return g_real.vkBeginCommandBuffer(cb, info);
}

void Barrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t nm,
             const VkMemoryBarrier* m, uint32_t nb, const VkBufferMemoryBarrier* b, uint32_t ni,
             const VkImageMemoryBarrier* im) {
  if (!IsProducer() || WithPNext(m, nm) || WithPNext(b, nb) || WithPNext(im, ni)) {
    if (IsProducer()) { DrainInternal(); ++g_direct; }
    g_real.vkCmdPipelineBarrier(cb, s, d, f, nm, m, nb, b, ni, im);
    return;
  }
  uint8_t* c = Arena(Size<VkMemoryBarrier>(nm) + Size<VkBufferMemoryBarrier>(nb) + Size<VkImageMemoryBarrier>(ni));
  const auto* m2 = Copy(c, m, nm);
  const auto* b2 = Copy(c, b, nb);
  const auto* i2 = Copy(c, im, ni);
  Enqueue(g_real.vkCmdPipelineBarrier, cb, s, d, f, nm, m2, nb, b2, ni, i2);
}

void BindSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n,
                 const VkDescriptorSet* sets, uint32_t nd, const uint32_t* dynamic_offsets) {
  if (!IsProducer()) return g_real.vkCmdBindDescriptorSets(cb, bp, l, first, n, sets, nd, dynamic_offsets);
  uint8_t* c = Arena(Size<VkDescriptorSet>(n) + Size<uint32_t>(nd));
  const auto* s2 = Copy(c, sets, n);
  const auto* d2 = Copy(c, dynamic_offsets, nd);
  Enqueue(g_real.vkCmdBindDescriptorSets, cb, bp, l, first, n, s2, nd, d2);
}

void Constants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size,
                const void* data) {
  if (!IsProducer()) return g_real.vkCmdPushConstants(cb, l, st, off, size, data);
  uint8_t* c = Arena(size);
  const auto* d2 = Copy(c, static_cast<const uint8_t*>(data), size);
  Enqueue(g_real.vkCmdPushConstants, cb, l, st, off, size, static_cast<const void*>(d2));
}

void BeginPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* info, VkSubpassContents content) {
  if (!IsProducer() || info->pNext) {
    if (IsProducer()) { DrainInternal(); ++g_direct; }
    return g_real.vkCmdBeginRenderPass(cb, info, content);
  }
  uint8_t* c = Arena(Size<VkRenderPassBeginInfo>(1) + Size<VkClearValue>(info->clearValueCount));
  auto* i2 = const_cast<VkRenderPassBeginInfo*>(Copy(c, info, 1));
  i2->pClearValues = Copy(c, info->pClearValues, info->clearValueCount);
  Enqueue(g_real.vkCmdBeginRenderPass, cb, static_cast<const VkRenderPassBeginInfo*>(i2), content);
}

void Framings(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v) {
  if (!IsProducer()) return g_real.vkCmdSetViewport(cb, first, n, v);
  uint8_t* c = Arena(Size<VkViewport>(n));
  Enqueue(g_real.vkCmdSetViewport, cb, first, n, Copy(c, v, n));
}
void Scissors(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* r) {
  if (!IsProducer()) return g_real.vkCmdSetScissor(cb, first, n, r);
  uint8_t* c = Arena(Size<VkRect2D>(n));
  Enqueue(g_real.vkCmdSetScissor, cb, first, n, Copy(c, r, n));
}
void ConstantsBlend(VkCommandBuffer cb, const float k[4]) {
  if (!IsProducer()) return g_real.vkCmdSetBlendConstants(cb, k);
  uint8_t* c = Arena(Size<float>(4));
  Enqueue(g_real.vkCmdSetBlendConstants, cb, Copy(c, k, 4));
}
void CopyBuffer(VkCommandBuffer cb, VkBuffer a, VkBuffer b, uint32_t n, const VkBufferCopy* r) {
  if (!IsProducer()) return g_real.vkCmdCopyBuffer(cb, a, b, n, r);
  uint8_t* c = Arena(Size<VkBufferCopy>(n));
  Enqueue(g_real.vkCmdCopyBuffer, cb, a, b, n, Copy(c, r, n));
}
void BufferToImage(VkCommandBuffer cb, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n,
                  const VkBufferImageCopy* r) {
  if (!IsProducer()) return g_real.vkCmdCopyBufferToImage(cb, b, i, l, n, r);
  uint8_t* c = Arena(Size<VkBufferImageCopy>(n));
  Enqueue(g_real.vkCmdCopyBufferToImage, cb, b, i, l, n, Copy(c, r, n));
}
void ImageToBuffer(VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n,
                  const VkBufferImageCopy* r) {
  if (!IsProducer()) return g_real.vkCmdCopyImageToBuffer(cb, i, l, b, n, r);
  uint8_t* c = Arena(Size<VkBufferImageCopy>(n));
  Enqueue(g_real.vkCmdCopyImageToBuffer, cb, i, l, b, n, Copy(c, r, n));
}
void ClearAttached(VkCommandBuffer cb, uint32_t na, const VkClearAttachment* a, uint32_t nr, const VkClearRect* r) {
  if (!IsProducer()) return g_real.vkCmdClearAttachments(cb, na, a, nr, r);
  uint8_t* c = Arena(Size<VkClearAttachment>(na) + Size<VkClearRect>(nr));
  const auto* a2 = Copy(c, a, na);
  const auto* r2 = Copy(c, r, nr);
  Enqueue(g_real.vkCmdClearAttachments, cb, na, a2, nr, r2);
}
void ClearColor(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue* v, uint32_t n,
                 const VkImageSubresourceRange* r) {
  if (!IsProducer()) return g_real.vkCmdClearColorImage(cb, i, l, v, n, r);
  uint8_t* c = Arena(Size<VkClearColorValue>(1) + Size<VkImageSubresourceRange>(n));
  const auto* v2 = Copy(c, v, 1);
  const auto* r2 = Copy(c, r, n);
  Enqueue(g_real.vkCmdClearColorImage, cb, i, l, v2, n, r2);
}
void BindVertices(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* b, const VkDeviceSize* o) {
  if (!IsProducer()) return g_real.vkCmdBindVertexBuffers(cb, first, n, b, o);
  uint8_t* c = Arena(Size<VkBuffer>(n) + Size<VkDeviceSize>(n));
  const auto* b2 = Copy(c, b, n);
  const auto* o2 = Copy(c, o, n);
  Enqueue(g_real.vkCmdBindVertexBuffers, cb, first, n, b2, o2);
}

// vkUpdateDescriptorSets queued (masseffect_deferred_native_update). Every array the call points to is copied into
// one arena block; the update then runs on the worker at the same position of the command stream, before the
// vkCmdBindDescriptorSets / draws recorded after it. Anything it cannot copy faithfully (pNext chains, a null array
// for a type that needs one, extension descriptor types) drains and runs on the ring thread as before.
void UpdateSets(VkDevice vulkan_device, uint32_t nw, const VkWriteDescriptorSet* w, uint32_t nc,
                    const VkCopyDescriptorSet* c) {
  if (!IsProducer()) return g_real.vkUpdateDescriptorSets(vulkan_device, nw, w, nc, c);
  size_t bytes = Size<VkWriteDescriptorSet>(nw) + Size<VkCopyDescriptorSet>(nc);
  bool simple = true;
  for (uint32_t i = 0; i < nw && simple; ++i) {
    const VkWriteDescriptorSet& e = w[i];
    if (e.pNext) {
      simple = false;
      break;
    }
    switch (e.descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        if (!e.pImageInfo) simple = false;
        else bytes += Size<VkDescriptorImageInfo>(e.descriptorCount);
        break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        if (!e.pTexelBufferView) simple = false;
        else bytes += Size<VkBufferView>(e.descriptorCount);
        break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        if (!e.pBufferInfo) simple = false;
        else bytes += Size<VkDescriptorBufferInfo>(e.descriptorCount);
        break;
      default:
        simple = false;
        break;
    }
  }
  for (uint32_t i = 0; i < nc && simple; ++i)
    if (c[i].pNext) simple = false;
  if (!simple || bytes > (size_t(64) << 10)) {
    DrainInternal();
    ++g_direct;
    ++g_direct_updates;
    return g_real.vkUpdateDescriptorSets(vulkan_device, nw, w, nc, c);
  }
  uint8_t* cursor = Arena(bytes);
  auto* w2 = reinterpret_cast<VkWriteDescriptorSet*>(cursor);
  cursor += Size<VkWriteDescriptorSet>(nw);
  if (nw) std::memcpy(w2, w, sizeof(VkWriteDescriptorSet) * nw);
  const auto* c2 = reinterpret_cast<const VkCopyDescriptorSet*>(cursor);
  if (nc) std::memcpy(cursor, c, sizeof(VkCopyDescriptorSet) * nc);
  cursor += Size<VkCopyDescriptorSet>(nc);
  for (uint32_t i = 0; i < nw; ++i) {
    VkWriteDescriptorSet& e = w2[i];
    switch (e.descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        e.pImageInfo = Copy(cursor, e.pImageInfo, e.descriptorCount);
        break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        e.pTexelBufferView = Copy(cursor, e.pTexelBufferView, e.descriptorCount);
        break;
      default:
        e.pBufferInfo = Copy(cursor, e.pBufferInfo, e.descriptorCount);
        break;
    }
  }
  ++g_updates;
  Enqueue(g_real.vkUpdateDescriptorSets, vulkan_device, nw, static_cast<const VkWriteDescriptorSet*>(w2), nc,
          nc ? c2 : static_cast<const VkCopyDescriptorSet*>(nullptr));
}

// Functions the renderer fetches by name.
PFN_vkCmdCopyImage g_copy_image = nullptr;
PFN_vkCmdBlitImage g_blit = nullptr;
PFN_vkCmdClearDepthStencilImage g_clear_depth = nullptr;
PFN_vkCmdSetColorBlendEnableEXT g_active_blend = nullptr;
PFN_vkCmdSetColorBlendEquationEXT g_equation = nullptr;
PFN_vkCmdSetColorWriteMaskEXT g_mask = nullptr;
PFN_vkCmdInsertDebugUtilsLabelEXT g_label = nullptr;

void CopyImage(VkCommandBuffer cb, VkImage a, VkImageLayout la, VkImage b, VkImageLayout lb, uint32_t n,
                  const VkImageCopy* r) {
  if (!IsProducer()) return g_copy_image(cb, a, la, b, lb, n, r);
  uint8_t* c = Arena(Size<VkImageCopy>(n));
  Enqueue(g_copy_image, cb, a, la, b, lb, n, Copy(c, r, n));
}
void Blit(VkCommandBuffer cb, VkImage a, VkImageLayout la, VkImage b, VkImageLayout lb, uint32_t n,
          const VkImageBlit* r, VkFilter f) {
  if (!IsProducer()) return g_blit(cb, a, la, b, lb, n, r, f);
  uint8_t* c = Arena(Size<VkImageBlit>(n));
  Enqueue(g_blit, cb, a, la, b, lb, n, Copy(c, r, n), f);
}
void ClearDepth(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue* v, uint32_t n,
                       const VkImageSubresourceRange* r) {
  if (!IsProducer()) return g_clear_depth(cb, i, l, v, n, r);
  uint8_t* c = Arena(Size<VkClearDepthStencilValue>(1) + Size<VkImageSubresourceRange>(n));
  const auto* v2 = Copy(c, v, 1);
  const auto* r2 = Copy(c, r, n);
  Enqueue(g_clear_depth, cb, i, l, v2, n, r2);
}
void ActiveBlend(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBool32* v) {
  if (!IsProducer()) return g_active_blend(cb, first, n, v);
  uint8_t* c = Arena(Size<VkBool32>(n));
  Enqueue(g_active_blend, cb, first, n, Copy(c, v, n));
}
void Equation(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkColorBlendEquationEXT* v) {
  if (!IsProducer()) return g_equation(cb, first, n, v);
  uint8_t* c = Arena(Size<VkColorBlendEquationEXT>(n));
  Enqueue(g_equation, cb, first, n, Copy(c, v, n));
}
void Mask(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkColorComponentFlags* v) {
  if (!IsProducer()) return g_mask(cb, first, n, v);
  uint8_t* c = Arena(Size<VkColorComponentFlags>(n));
  Enqueue(g_mask, cb, first, n, Copy(c, v, n));
}
void Label(VkCommandBuffer cb, const VkDebugUtilsLabelEXT* e) {
  if (!IsProducer() || e->pNext) {
    if (IsProducer()) { DrainInternal(); ++g_direct; }
    return g_label(cb, e);
  }
  const size_t long_value = e->pLabelName ? std::strlen(e->pLabelName) + 1 : 0;
  uint8_t* c = Arena(Size<VkDebugUtilsLabelEXT>(1) + Size<char>(uint32_t(long_value)));
  auto* e2 = const_cast<VkDebugUtilsLabelEXT*>(Copy(c, e, 1));
  if (long_value) e2->pLabelName = Copy(c, e->pLabelName, uint32_t(long_value));
  Enqueue(g_label, cb, static_cast<const VkDebugUtilsLabelEXT*>(e2));
}

template <size_t Id, typename PFN>
PFN_vkVoidFunction Generic(PFN_vkVoidFunction real, const char* name) {
  using E = Wrapper<Id, PFN>;
  E::real = reinterpret_cast<PFN>(real);
  E::name = name;
  E::mode = E::kDeferrable ? kDefer : kDrain;
  return reinterpret_cast<PFN_vkVoidFunction>(&E::F);
}

}  // namespace

static const Functions& DeferredTable(const Functions& real_fns) {
  if (!REXCVAR_GET(masseffect_native_deferred_recording)) return real_fns;
  static const bool built = [&] {
    g_real = real_fns;
    g_proxy = real_fns;
    const int32_t parts = REXCVAR_GET(masseffect_native_deferred_recording_parts);
    if (!(parts & 4)) {
#define XE_UI_VULKAN_FUNCTION(name) Install<__COUNTER__>(real_fns.name, g_proxy.name, #name);
#define XE_UI_VULKAN_FUNCTION_PROMOTED(extension_name, core_name) \
  Install<__COUNTER__>(real_fns.core_name, g_proxy.core_name, #core_name);
#include <rex/ui/vulkan/functions/device_1_0.inc>
#include <rex/ui/vulkan/functions/device_khr_swapchain.inc>
#include <rex/ui/vulkan/functions/device_1_1_khr_get_memory_requirements2.inc>
#include <rex/ui/vulkan/functions/device_1_1_khr_bind_memory2.inc>
#include <rex/ui/vulkan/functions/device_1_3_khr_maintenance4.inc>
#include <rex/ui/vulkan/functions/device_1_3_khr_dynamic_rendering.inc>
#undef XE_UI_VULKAN_FUNCTION_PROMOTED
#undef XE_UI_VULKAN_FUNCTION
    }
    if (!(parts & 8)) {
    // The calls with arrays: their own wrappers (the generic ones would only drain and call directly).
    if (!(parts & 16)) g_proxy.vkBeginCommandBuffer = &BeginCommands;
    g_no_enqueue = (parts & 32) != 0;
    g_proxy.vkCmdPipelineBarrier = &Barrier;
    g_proxy.vkCmdBindDescriptorSets = &BindSets;
    g_proxy.vkCmdPushConstants = &Constants;
    g_proxy.vkCmdBeginRenderPass = &BeginPass;
    g_proxy.vkCmdSetViewport = &Framings;
    g_proxy.vkCmdSetScissor = &Scissors;
    g_proxy.vkCmdSetBlendConstants = &ConstantsBlend;
    g_proxy.vkCmdCopyBuffer = &CopyBuffer;
    g_proxy.vkCmdCopyBufferToImage = &BufferToImage;
    g_proxy.vkCmdCopyImageToBuffer = &ImageToBuffer;
    g_proxy.vkCmdClearAttachments = &ClearAttached;
    g_proxy.vkCmdClearColorImage = &ClearColor;
    g_proxy.vkCmdBindVertexBuffers = &BindVertices;
    if (REXCVAR_GET(masseffect_deferred_native_update)) g_proxy.vkUpdateDescriptorSets = &UpdateSets;
    }
    g_fast = REXCVAR_GET(masseffect_deferred_native_fast);
    g_packets = REXCVAR_GET(banjo_deferred_packets);
    g_batch = g_fast ? uint32_t(REXCVAR_GET(masseffect_deferred_native_batch)) : 1;
    g_queue = new (std::nothrow) Slot[kSlots];
    g_arena = new (std::nothrow) uint8_t[kArena];
    if (!g_queue || !g_arena) {
      REXLOG_ERROR("[native] deferred recording DISABLED: out of memory for the queue ({} KB)",
                   (sizeof(Slot) * kSlots + kArena) / 1024);
      return false;
    }
    g_active.store(true, std::memory_order_release);
    REXLOG_INFO("[native] deferred recording ready: {} queue slots, {} MB arena", kSlots,
                kArena >> 20);
    return true;
  }();
  return built && (REXCVAR_GET(masseffect_native_deferred_recording_parts) & 1) ? g_proxy : real_fns;
}

// ---------------------------------------------------------------------------------------------------------------
// banjo_diag_vk_calls: counters around every vkCmd* the renderer calls (through Table and Proc), on top of whatever
// the table is (the deferred proxy or the driver's functions). Measurement only: each wrapper counts and calls on.
namespace {
constexpr size_t kCountSlots = 256;
std::atomic<uint64_t> g_count_calls[kCountSlots];
const char* g_count_names[kCountSlots];
std::atomic<size_t> g_count_used{0};
bool g_counting_active = false;
Functions g_counting;

size_t CountSlot(const char* name) {
  for (size_t i = 0; i < g_count_used.load(std::memory_order_acquire); ++i)
    if (g_count_names[i] == name || std::string_view(g_count_names[i]) == name) return i;
  const size_t i = g_count_used.load(std::memory_order_relaxed);
  if (i >= kCountSlots) return kCountSlots - 1;
  g_count_names[i] = name;
  g_count_used.store(i + 1, std::memory_order_release);
  return i;
}

template <size_t Id, typename PFN>
struct Counter;
template <size_t Id, typename R, typename... A>
struct Counter<Id, R (*)(A...)> {
  static inline R (*real)(A...) = nullptr;
  static inline size_t slot = kCountSlots - 1;
  static R F(A... a) {
    g_count_calls[slot].fetch_add(1, std::memory_order_relaxed);
    return real(a...);
  }
};

template <size_t Id, typename PFN>
void InstallCounter(PFN real, PFN& target, const char* name) {
  if (!real || !std::string_view(name).starts_with("vkCmd")) return;
  using C = Counter<Id, PFN>;
  C::real = real;
  C::slot = CountSlot(name);
  target = &C::F;
}

template <size_t Id, typename PFN>
PFN_vkVoidFunction CounterByName(PFN_vkVoidFunction real, const char* name) {
  using C = Counter<Id, PFN>;
  C::real = reinterpret_cast<PFN>(real);
  C::slot = CountSlot(name);
  return reinterpret_cast<PFN_vkVoidFunction>(&C::F);
}

// Call sites by return address (the instruction after the renderer's call), for the barriers and render passes:
// which code issues them. Reported relative to ReportCalls (tools: its symbol address + the offset = the site).
struct SiteTable {
  static constexpr size_t kSites = 128;
  std::mutex mutex;
  uintptr_t address[kSites] = {};
  uint64_t calls[kSites] = {};
  uint64_t heavy[kSites] = {};
  uint64_t overflow = 0;
  void Note(uintptr_t at, bool is_heavy) {
    std::lock_guard<std::mutex> lock(mutex);
    for (size_t i = 0; i < kSites; ++i) {
      if (address[i] == at || address[i] == 0) {
        address[i] = at;
        ++calls[i];
        heavy[i] += is_heavy ? 1 : 0;
        return;
      }
    }
    ++overflow;
  }
};
SiteTable g_barrier_sites, g_pass_sites;

struct BarrierCounts {
  std::atomic<uint64_t> calls{0}, images{0}, layouts{0}, buffers{0}, memories{0}, heavy{0}, all_commands{0},
      to_shader_read{0};
} g_barriers;
PFN_vkCmdPipelineBarrier g_counted_barrier = nullptr;
bool HeavyAccess(VkAccessFlags f) { return (f & (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)) != 0; }
void CountedBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep,
                    uint32_t n_memory, const VkMemoryBarrier* memory, uint32_t n_buffer,
                    const VkBufferMemoryBarrier* buffer, uint32_t n_image, const VkImageMemoryBarrier* image) {
  g_barriers.calls.fetch_add(1, std::memory_order_relaxed);
  g_barriers.memories.fetch_add(n_memory, std::memory_order_relaxed);
  g_barriers.buffers.fetch_add(n_buffer, std::memory_order_relaxed);
  g_barriers.images.fetch_add(n_image, std::memory_order_relaxed);
  bool heavy = false;
  for (uint32_t i = 0; i < n_memory; ++i) heavy |= HeavyAccess(memory[i].srcAccessMask | memory[i].dstAccessMask);
  for (uint32_t i = 0; i < n_buffer; ++i) heavy |= HeavyAccess(buffer[i].srcAccessMask | buffer[i].dstAccessMask);
  uint64_t layouts = 0, to_read = 0;
  for (uint32_t i = 0; i < n_image; ++i) {
    heavy |= HeavyAccess(image[i].srcAccessMask | image[i].dstAccessMask);
    layouts += image[i].oldLayout != image[i].newLayout ? 1 : 0;
    to_read += image[i].newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? 1 : 0;
  }
  g_barriers.layouts.fetch_add(layouts, std::memory_order_relaxed);
  g_barriers.to_shader_read.fetch_add(to_read, std::memory_order_relaxed);
  if (heavy) g_barriers.heavy.fetch_add(1, std::memory_order_relaxed);
  g_barrier_sites.Note(reinterpret_cast<uintptr_t>(__builtin_return_address(0)), heavy);
  if ((src | dst) & (VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT))
    g_barriers.all_commands.fetch_add(1, std::memory_order_relaxed);
  g_counted_barrier(cb, src, dst, dep, n_memory, memory, n_buffer, buffer, n_image, image);
}

// Render passes by what their attachments load and store.
struct PassShape {
  int8_t depth_load = -1, depth_store = -1, stencil_load = -1;  // -1: no depth attachment
  uint8_t color_load[3] = {};  // LOAD, CLEAR, DONT_CARE
  uint8_t colors = 0;
  uint8_t samples = 1;
};
std::mutex g_pass_mutex;
std::unordered_map<VkRenderPass, PassShape> g_pass_shapes;
PFN_vkCreateRenderPass g_counted_create_pass = nullptr;
PFN_vkCmdBeginRenderPass g_counted_begin_pass = nullptr;
struct PassCounts {
  std::atomic<uint64_t> passes{0}, unknown{0}, no_depth{0}, depth_load[3]{}, depth_store[2]{}, stencil_load[3]{},
      color_load[3]{}, msaa{0};
} g_pass_counts;
int8_t LoadIndex(VkAttachmentLoadOp op) {
  return op == VK_ATTACHMENT_LOAD_OP_LOAD ? 0 : op == VK_ATTACHMENT_LOAD_OP_CLEAR ? 1 : 2;
}
VkResult CountedCreatePass(VkDevice device, const VkRenderPassCreateInfo* info, const VkAllocationCallbacks* allocator,
                           VkRenderPass* pass) {
  const VkResult result = g_counted_create_pass(device, info, allocator, pass);
  if (result == VK_SUCCESS && info && info->subpassCount) {
    PassShape shape;
    const VkSubpassDescription& sub = info->pSubpasses[0];
    if (sub.pDepthStencilAttachment && sub.pDepthStencilAttachment->attachment < info->attachmentCount) {
      const VkAttachmentDescription& d = info->pAttachments[sub.pDepthStencilAttachment->attachment];
      shape.depth_load = LoadIndex(d.loadOp);
      shape.depth_store = d.storeOp == VK_ATTACHMENT_STORE_OP_STORE ? 0 : 1;
      shape.stencil_load = LoadIndex(d.stencilLoadOp);
      shape.samples = uint8_t(d.samples);
    }
    for (uint32_t i = 0; i < sub.colorAttachmentCount; ++i) {
      const uint32_t a = sub.pColorAttachments[i].attachment;
      if (a >= info->attachmentCount) continue;
      ++shape.color_load[LoadIndex(info->pAttachments[a].loadOp)];
      ++shape.colors;
      if (info->pAttachments[a].samples > shape.samples) shape.samples = uint8_t(info->pAttachments[a].samples);
    }
    std::lock_guard<std::mutex> lock(g_pass_mutex);
    g_pass_shapes[*pass] = shape;
  }
  return result;
}
void CountedBeginPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* begin, VkSubpassContents contents) {
  g_pass_counts.passes.fetch_add(1, std::memory_order_relaxed);
  g_pass_sites.Note(reinterpret_cast<uintptr_t>(__builtin_return_address(0)), false);
  PassShape shape;
  bool known = false;
  {
    std::lock_guard<std::mutex> lock(g_pass_mutex);
    const auto it = g_pass_shapes.find(begin->renderPass);
    if (it != g_pass_shapes.end()) {
      shape = it->second;
      known = true;
    }
  }
  if (!known) {
    g_pass_counts.unknown.fetch_add(1, std::memory_order_relaxed);
  } else {
    if (shape.depth_load < 0) {
      g_pass_counts.no_depth.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_pass_counts.depth_load[shape.depth_load].fetch_add(1, std::memory_order_relaxed);
      g_pass_counts.depth_store[shape.depth_store].fetch_add(1, std::memory_order_relaxed);
      g_pass_counts.stencil_load[shape.stencil_load].fetch_add(1, std::memory_order_relaxed);
    }
    for (int i = 0; i < 3; ++i) g_pass_counts.color_load[i].fetch_add(shape.color_load[i], std::memory_order_relaxed);
    if (shape.samples > 1) g_pass_counts.msaa.fetch_add(1, std::memory_order_relaxed);
  }
  g_counted_begin_pass(cb, begin, contents);
}

const Functions& CountingTable(const Functions& base) {
  static const bool built = [&] {
    g_counting = base;
#define XE_UI_VULKAN_FUNCTION(name) InstallCounter<__COUNTER__>(base.name, g_counting.name, #name);
#define XE_UI_VULKAN_FUNCTION_PROMOTED(extension_name, core_name) \
  InstallCounter<__COUNTER__>(base.core_name, g_counting.core_name, #core_name);
#include <rex/ui/vulkan/functions/device_1_0.inc>
#include <rex/ui/vulkan/functions/device_khr_swapchain.inc>
#include <rex/ui/vulkan/functions/device_1_1_khr_get_memory_requirements2.inc>
#include <rex/ui/vulkan/functions/device_1_1_khr_bind_memory2.inc>
#include <rex/ui/vulkan/functions/device_1_3_khr_maintenance4.inc>
#include <rex/ui/vulkan/functions/device_1_3_khr_dynamic_rendering.inc>
#undef XE_UI_VULKAN_FUNCTION_PROMOTED
#undef XE_UI_VULKAN_FUNCTION
    // Their own wrappers (after the generic ones, which they replace), counted in the same slots.
    if (base.vkCmdPipelineBarrier) {
      g_counted_barrier = base.vkCmdPipelineBarrier;
      g_counting.vkCmdPipelineBarrier = &CountedBarrier;
    }
    if (base.vkCreateRenderPass) {
      g_counted_create_pass = base.vkCreateRenderPass;
      g_counting.vkCreateRenderPass = &CountedCreatePass;
    }
    if (base.vkCmdBeginRenderPass) {
      g_counted_begin_pass = base.vkCmdBeginRenderPass;
      g_counting.vkCmdBeginRenderPass = &CountedBeginPass;
    }
    g_counting_active = true;
    REXLOG_INFO("[native] Vulkan command counters on (banjo_diag_vk_calls): {} vkCmd functions in the table",
                g_count_used.load());
    return true;
  }();
  return built ? g_counting : base;
}
}  // namespace

const Functions& Table(const Functions& real_fns) {
  const Functions& base = DeferredTable(real_fns);
  if (!REXCVAR_GET(banjo_diag_vk_calls)) return base;
  return CountingTable(base);
}

void ReportCalls(uint64_t swaps) {
  if (!g_counting_active || !swaps) return;
  std::vector<std::pair<uint64_t, const char*>> calls;
  const size_t used = g_count_used.load(std::memory_order_acquire);
  for (size_t i = 0; i < used; ++i) {
    const uint64_t n = g_count_calls[i].exchange(0, std::memory_order_relaxed);
    if (n) calls.emplace_back(n, g_count_names[i]);
  }
  const uint64_t barriers = g_barriers.calls.exchange(0, std::memory_order_relaxed);
  if (barriers) calls.emplace_back(barriers, "vkCmdPipelineBarrier");
  const uint64_t passes = g_pass_counts.passes.exchange(0, std::memory_order_relaxed);
  if (passes) calls.emplace_back(passes, "vkCmdBeginRenderPass");
  std::sort(calls.begin(), calls.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::string text;
  uint64_t total = 0;
  for (const auto& [n, name] : calls) total += n;
  for (size_t i = 0; i < calls.size() && i < 32; ++i)
    text += fmt::format(" {} {:.1f},", calls[i].second, double(calls[i].first) / double(swaps));
  if (!text.empty()) text.pop_back();
  const double f = 1.0 / double(swaps);
  REXLOG_INFO("[native] Vulkan commands per Swap (banjo_diag_vk_calls, {} Swaps): {:.0f} in all;{}", swaps,
              double(total) * f, text);
  auto take = [](std::atomic<uint64_t>& a) { return double(a.exchange(0, std::memory_order_relaxed)); };
  REXLOG_INFO("[native] barriers per Swap (banjo_diag_vk_calls): {:.1f} vkCmdPipelineBarrier calls, {:.1f} image "
              "barriers ({:.1f} change the layout, {:.1f} to SHADER_READ_ONLY), {:.1f} buffer, {:.1f} memory; {:.1f} "
              "calls with MEMORY_READ / MEMORY_WRITE access (a full cache flush on NVK), {:.1f} with ALL_COMMANDS or "
              "ALL_GRAPHICS stages",
              double(barriers) * f, take(g_barriers.images) * f, take(g_barriers.layouts) * f,
              take(g_barriers.to_shader_read) * f, take(g_barriers.buffers) * f, take(g_barriers.memories) * f,
              take(g_barriers.heavy) * f, take(g_barriers.all_commands) * f);
  REXLOG_INFO("[native] render passes per Swap (banjo_diag_vk_calls): {:.1f} begun ({:.1f} multisampled, {:.1f} of "
              "an unknown render pass); depth load {:.1f} / clear {:.1f} / don't care {:.1f}, store {:.1f} / don't "
              "care {:.1f}, none {:.1f}; stencil load {:.1f} / clear {:.1f} / don't care {:.1f}; colour attachments "
              "load {:.1f} / clear {:.1f} / don't care {:.1f}",
              double(passes) * f, take(g_pass_counts.msaa) * f, take(g_pass_counts.unknown) * f,
              take(g_pass_counts.depth_load[0]) * f, take(g_pass_counts.depth_load[1]) * f,
              take(g_pass_counts.depth_load[2]) * f, take(g_pass_counts.depth_store[0]) * f,
              take(g_pass_counts.depth_store[1]) * f, take(g_pass_counts.no_depth) * f,
              take(g_pass_counts.stencil_load[0]) * f, take(g_pass_counts.stencil_load[1]) * f,
              take(g_pass_counts.stencil_load[2]) * f, take(g_pass_counts.color_load[0]) * f,
              take(g_pass_counts.color_load[1]) * f, take(g_pass_counts.color_load[2]) * f);
  // The sites, most calls first; offsets from ReportCalls (masseffect::native::deferred::ReportCalls in the ELF / PDB).
  const uintptr_t anchor = reinterpret_cast<uintptr_t>(&ReportCalls);
  auto sites = [&](SiteTable& table, const char* what) {
    std::vector<std::tuple<uint64_t, uint64_t, intptr_t>> list;
    {
      std::lock_guard<std::mutex> lock(table.mutex);
      for (size_t i = 0; i < SiteTable::kSites && table.address[i]; ++i) {
        if (table.calls[i]) list.emplace_back(table.calls[i], table.heavy[i], intptr_t(table.address[i] - anchor));
        table.calls[i] = table.heavy[i] = 0;
      }
    }
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
    std::string text;
    for (size_t i = 0; i < list.size() && i < 16; ++i)
      text += fmt::format(" | ReportCalls{:+#x}: {:.1f} ({:.1f} with MEMORY access)", std::get<2>(list[i]),
                          double(std::get<0>(list[i])) * f, double(std::get<1>(list[i])) * f);
    if (!text.empty())
      REXLOG_INFO("[native] {} call sites per Swap (banjo_diag_vk_calls; return addresses relative to "
                  "masseffect::native::deferred::ReportCalls){}", what, text);
  };
  sites(g_barrier_sites, "vkCmdPipelineBarrier");
  sites(g_pass_sites, "vkCmdBeginRenderPass");
}

static PFN_vkVoidFunction DeferredProc(PFN_vkGetDeviceProcAddr real, VkDevice device, const char* name);

PFN_vkVoidFunction Proc(PFN_vkGetDeviceProcAddr real, VkDevice device, const char* name) {
  const PFN_vkVoidFunction f = DeferredProc(real, device, name);
  if (!f || !REXCVAR_GET(banjo_diag_vk_calls)) return f;
  const std::string_view n(name);
#define COUNT_BY_NAME(fn_name) \
  if (n == #fn_name) return CounterByName<__COUNTER__, PFN_##fn_name>(f, #fn_name);
  COUNT_BY_NAME(vkCmdCopyImage)
  COUNT_BY_NAME(vkCmdBlitImage)
  COUNT_BY_NAME(vkCmdClearDepthStencilImage)
  COUNT_BY_NAME(vkCmdSetColorBlendEnableEXT)
  COUNT_BY_NAME(vkCmdSetColorBlendEquationEXT)
  COUNT_BY_NAME(vkCmdSetColorWriteMaskEXT)
  COUNT_BY_NAME(vkCmdInsertDebugUtilsLabelEXT)
  COUNT_BY_NAME(vkCmdWriteTimestamp)
  COUNT_BY_NAME(vkCmdSetCullMode)
  COUNT_BY_NAME(vkCmdSetFrontFace)
  COUNT_BY_NAME(vkCmdSetPrimitiveTopology)
  COUNT_BY_NAME(vkCmdSetPrimitiveRestartEnable)
  COUNT_BY_NAME(vkCmdSetDepthBiasEnable)
  COUNT_BY_NAME(vkCmdSetDepthTestEnable)
  COUNT_BY_NAME(vkCmdSetDepthWriteEnable)
  COUNT_BY_NAME(vkCmdSetDepthCompareOp)
  COUNT_BY_NAME(vkCmdSetStencilTestEnable)
  COUNT_BY_NAME(vkCmdSetStencilOp)
#undef COUNT_BY_NAME
  return f;
}

static PFN_vkVoidFunction DeferredProc(PFN_vkGetDeviceProcAddr real, VkDevice device, const char* name) {
  const PFN_vkVoidFunction f = real(device, name);
  if (!f || !g_active.load(std::memory_order_acquire) ||
      !(REXCVAR_GET(masseffect_native_deferred_recording_parts) & 2))
    return f;
  const std::string_view n(name);
  if (n == "vkCmdCopyImage") { g_copy_image = reinterpret_cast<PFN_vkCmdCopyImage>(f); return reinterpret_cast<PFN_vkVoidFunction>(&CopyImage); }
  if (n == "vkCmdBlitImage") { g_blit = reinterpret_cast<PFN_vkCmdBlitImage>(f); return reinterpret_cast<PFN_vkVoidFunction>(&Blit); }
  if (n == "vkCmdClearDepthStencilImage") { g_clear_depth = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(f); return reinterpret_cast<PFN_vkVoidFunction>(&ClearDepth); }
  if (n == "vkCmdSetColorBlendEnableEXT") { g_active_blend = reinterpret_cast<PFN_vkCmdSetColorBlendEnableEXT>(f); return reinterpret_cast<PFN_vkVoidFunction>(&ActiveBlend); }
  if (n == "vkCmdSetColorBlendEquationEXT") { g_equation = reinterpret_cast<PFN_vkCmdSetColorBlendEquationEXT>(f); return reinterpret_cast<PFN_vkVoidFunction>(&Equation); }
  if (n == "vkCmdSetColorWriteMaskEXT") { g_mask = reinterpret_cast<PFN_vkCmdSetColorWriteMaskEXT>(f); return reinterpret_cast<PFN_vkVoidFunction>(&Mask); }
  if (n == "vkCmdInsertDebugUtilsLabelEXT") { g_label = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(f); return reinterpret_cast<PFN_vkVoidFunction>(&Label); }
#define GENERIC(fn_name) \
  if (n == #fn_name) return Generic<__COUNTER__, PFN_##fn_name>(f, #fn_name);
  GENERIC(vkCmdWriteTimestamp)
  GENERIC(vkCmdSetCullMode)
  GENERIC(vkCmdSetFrontFace)
  GENERIC(vkCmdSetPrimitiveTopology)
  GENERIC(vkCmdSetPrimitiveRestartEnable)
  GENERIC(vkCmdSetDepthBiasEnable)
  GENERIC(vkCmdSetDepthTestEnable)
  GENERIC(vkCmdSetDepthWriteEnable)
  GENERIC(vkCmdSetDepthCompareOp)
  GENERIC(vkCmdSetStencilTestEnable)
  GENERIC(vkCmdSetStencilOp)
  GENERIC(vkGetQueryPoolResults)
#undef GENERIC
  if (n.starts_with("vkCmd")) {
    // A recording call this proxy does not know would run out of order: no deferral at all.
    g_active.store(false, std::memory_order_release);
    REXLOG_ERROR("[native] deferred recording DISABLED: no wrapper for {}", name);
  }
  return f;
}

void Drain() {
  if (IsProducer()) {
    g_reason = "Drain() (the renderer)";
    DrainInternal();
  }
}

void Flush() {
  if (!IsProducer()) return;
  ClosePacket();  // banjo_deferred_packets
  if (g_fast && g_published != g_written_producer) PublishAlready();
}

// banjo_deferred_packets: see the header.
bool PacketBindPipeline(VkCommandBuffer cb, VkPipeline pipeline) {
  if (!PacketsTake()) return false;
  uint8_t* p = PacketSpace(cb, 16);
  Put<uint32_t>(p, kOpBindPipeline);
  Put<uint32_t>(p, 16);
  Put<VkPipeline>(p, pipeline);
  return true;
}
bool PacketBindSets(VkCommandBuffer cb, VkPipelineLayout layout, uint32_t first, uint32_t n,
                    const VkDescriptorSet* sets, uint32_t nd, const uint32_t* dynamic_offsets) {
  if (!PacketsTake() || n > 8 || nd > 8) return false;
  const size_t bytes = Up8(8 + sizeof(VkPipelineLayout) + 16 + sizeof(VkDescriptorSet) * n + 4 * nd);
  uint8_t* p = PacketSpace(cb, bytes);
  Put<uint32_t>(p, kOpBindSets);
  Put<uint32_t>(p, uint32_t(bytes));
  Put<VkPipelineLayout>(p, layout);
  Put<uint32_t>(p, first);
  Put<uint32_t>(p, n);
  Put<uint32_t>(p, nd);
  Put<uint32_t>(p, 0);
  for (uint32_t i = 0; i < n; ++i) Put<VkDescriptorSet>(p, sets[i]);
  for (uint32_t i = 0; i < nd; ++i) Put<uint32_t>(p, dynamic_offsets[i]);
  return true;
}
bool PacketPush(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset,
                uint32_t size, const void* data) {
  if (!PacketsTake() || size > 256) return false;
  const size_t bytes = Up8(8 + sizeof(VkPipelineLayout) + 16 + size);
  uint8_t* p = PacketSpace(cb, bytes);
  Put<uint32_t>(p, kOpPush);
  Put<uint32_t>(p, uint32_t(bytes));
  Put<VkPipelineLayout>(p, layout);
  Put<uint32_t>(p, stages);
  Put<uint32_t>(p, offset);
  Put<uint32_t>(p, size);
  Put<uint32_t>(p, 0);
  std::memcpy(p, data, size);
  return true;
}
bool PacketViewport(VkCommandBuffer cb, const VkViewport& v) {
  if (!PacketsTake()) return false;
  constexpr size_t bytes = (8 + sizeof(VkViewport) + 7) & ~size_t(7);
  uint8_t* p = PacketSpace(cb, bytes);
  Put<uint32_t>(p, kOpViewport);
  Put<uint32_t>(p, uint32_t(bytes));
  Put<VkViewport>(p, v);
  return true;
}
bool PacketScissor(VkCommandBuffer cb, const VkRect2D& r) {
  if (!PacketsTake()) return false;
  constexpr size_t bytes = (8 + sizeof(VkRect2D) + 7) & ~size_t(7);
  uint8_t* p = PacketSpace(cb, bytes);
  Put<uint32_t>(p, kOpScissor);
  Put<uint32_t>(p, uint32_t(bytes));
  Put<VkRect2D>(p, r);
  return true;
}
bool PacketBlend(VkCommandBuffer cb, const float k[4]) {
  if (!PacketsTake()) return false;
  uint8_t* p = PacketSpace(cb, 24);
  Put<uint32_t>(p, kOpBlend);
  Put<uint32_t>(p, 24);
  for (int i = 0; i < 4; ++i) Put<float>(p, k[i]);
  return true;
}
bool PacketBias(VkCommandBuffer cb, float constant, float clamp, float slope) {
  if (!PacketsTake()) return false;
  uint8_t* p = PacketSpace(cb, 24);
  Put<uint32_t>(p, kOpBias);
  Put<uint32_t>(p, 24);
  Put<float>(p, constant);
  Put<float>(p, clamp);
  Put<float>(p, slope);
  return true;
}
bool PacketStencil(VkCommandBuffer cb, uint32_t front, uint32_t back) {
  if (!PacketsTake()) return false;
  uint8_t* p = PacketSpace(cb, 16);
  Put<uint32_t>(p, kOpStencil);
  Put<uint32_t>(p, 16);
  Put<uint32_t>(p, front);
  Put<uint32_t>(p, back);
  return true;
}
bool PacketVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* buffers,
                         const VkDeviceSize* offsets) {
  if (!PacketsTake() || n > 32) return false;
  const size_t bytes = Up8(16 + (sizeof(VkBuffer) + sizeof(VkDeviceSize)) * n);
  uint8_t* p = PacketSpace(cb, bytes);
  Put<uint32_t>(p, kOpVertexBuffers);
  Put<uint32_t>(p, uint32_t(bytes));
  Put<uint32_t>(p, first);
  Put<uint32_t>(p, n);
  for (uint32_t i = 0; i < n; ++i) Put<VkBuffer>(p, buffers[i]);
  for (uint32_t i = 0; i < n; ++i) Put<VkDeviceSize>(p, offsets[i]);
  return true;
}
bool PacketIndexBuffer(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
  if (!PacketsTake()) return false;
  constexpr size_t bytes = 16 + sizeof(VkBuffer) + sizeof(VkDeviceSize);
  uint8_t* p = PacketSpace(cb, bytes);
  Put<uint32_t>(p, kOpIndexBuffer);
  Put<uint32_t>(p, uint32_t(bytes));
  Put<uint32_t>(p, uint32_t(type));
  Put<uint32_t>(p, 0);
  Put<VkBuffer>(p, buffer);
  Put<VkDeviceSize>(p, offset);
  return true;
}
bool PacketDrawIndexed(VkCommandBuffer cb, uint32_t count, uint32_t instances, uint32_t first, int32_t vertex_offset,
                       uint32_t first_instance) {
  if (!PacketsTake()) return false;
  uint8_t* p = PacketSpace(cb, 32);
  Put<uint32_t>(p, kOpDrawIndexed);
  Put<uint32_t>(p, 32);
  Put<uint32_t>(p, count);
  Put<uint32_t>(p, instances);
  Put<uint32_t>(p, first);
  Put<int32_t>(p, vertex_offset);
  Put<uint32_t>(p, first_instance);
  return true;
}
bool PacketDraw(VkCommandBuffer cb, uint32_t count, uint32_t instances, uint32_t first, uint32_t first_instance) {
  if (!PacketsTake()) return false;
  uint8_t* p = PacketSpace(cb, 24);
  Put<uint32_t>(p, kOpDraw);
  Put<uint32_t>(p, 24);
  Put<uint32_t>(p, count);
  Put<uint32_t>(p, instances);
  Put<uint32_t>(p, first);
  Put<uint32_t>(p, first_instance);
  return true;
}

// banjo-nx: the in-run A/B (banjo_ab_target = 16). With `direct` the producer makes every call itself, as if the layer
// were off; the queue is emptied first, so the commands keep their order across the switch.
void SetDirect(bool direct) {
  if (!IsProducer() || g_no_enqueue == direct) return;
  DrainInternal();
  g_no_enqueue = direct;
}

}  // namespace masseffect::native::deferred
