// banjo-nx - Banjo-Kazooie: Nuts & Bolts hooks for the native renderer (from masseffect-nx's me_d3d_trace.cpp and
// me_shader_dump.cpp, with Banjo's addresses).
//
// The renderer identifies each draw's shaders from the PM4 ring by itself (IM_LOAD microcode: by content, by the
// microcode's address, or masked against the library). These hooks only feed it what the ring cannot say:
//
//   - Shader objects. NoteShaderObject reads the container copy inside a D3D shader object (VS +0x368, PS +0x28)
//     and its microcode pointer (VS +0x20, PS +0x18), and maps the microcode's address and content to the library
//     entry. Banjo's draw-time shader load is sub_821FAF10 (r3 = device, r4 = dirty bits; the counterpart of Mass
//     Effect's sub_821FE318): it reads the bound VS from device+12696 and the PS from device+12692 (Mass Effect:
//     +12416 / +12412), so the objects are noted there, before the original patches the VS for the declaration.
//     Mass Effect also hooks the link/patch call (sub_8222F850, r3 = VS, r7 = PS); in Banjo, sub_821FAF10 calls
//     0x8263FA70 with r3 = device instead, so the draw-time hook alone covers it.
//   - Draw records, a hint for pairing (NoteDrawCall: primitive type, then r5..r7 as D3D received them):
//       D3DDevice_DrawVertices        0x821FB880 (dev, type, start, count)
//       D3DDevice_DrawIndexedVertices 0x8263F358 (dev, type, base, start, count)
//       D3DDevice_DrawVerticesUP      0x821FD698 (dev, type, count, data, stride)
//   - The game frame counter (D3DDevice_Swap, 0x822554F8), for the overlay and the hang watch.
//   - banjo_direct_state (the direct renderer, step 1, from sa2-nx): each draw hook also hands the state packets the
//     draw function writes to the ring as a host record (BeginDrawDirect before the original, EndDrawDirect right
//     after it; r3 = the device: +48 the command buffer write pointer, +52/+56 the segment's limits).
//
//   - banjo_d3d_records (step 4 of docs/d3d-full-replacement.md): the same for whole calls of every Direct3D function
//     that writes packets: the draw hooks, Swap and Resolve here, and the generated hooks of
//     banjo_d3d_record_hooks.inc (tools/d3d/d3d_record_hooks.py) on the other writers. A counter in the thread's guest
//     context (host_counters[63]) makes only the outermost call convert.
//
//   - banjo_d3d_native_draw_indexed / _draw_vertices (me/banjo_d3d_native.h, from sa2-nx's native Direct3D layer):
//     DrawIndexedVertices and DrawVertices with their draw-time state flush as native code that writes the same
//     packets; the hook runs the original when the native code declines the call.
//   - banjo_diag_d3d_census (me/banjo_d3d_census.h): a diagnostic count of Direct3D's calls and packets.
//
// The other hooks call the original; none changes a PPC register.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "me/banjo_d3d_census.h"
#include "me/banjo_d3d_native.h"
#include "me/banjo_d3d_clear.h"
#include "me/banjo_d3d_locks.h"
#include "me/me_native_system.h"
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/banjo_shared_hooks.h"

std::atomic<uint64_t> g_banjo_game_frames{0};

namespace {

inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

// The game's frame rate every 10 s (Swaps), with either renderer: the one figure the two are compared by in a log.
void ReportFrameRate(std::chrono::steady_clock::time_point now) {
  static auto last = now;
  static uint64_t last_frames = 0;
  const uint64_t frames = g_banjo_game_frames.load(std::memory_order_relaxed);
  const double seconds = std::chrono::duration<double>(now - last).count();
  if (seconds > 0.0)
    REXLOG_INFO("[frames] {} game frames in {:.1f} s: {:.2f} fps ({} renderer)", frames - last_frames, seconds,
                double(frames - last_frames) / seconds, ::banjo::renut_gpu::Active() ? "Renut's" : "banjo-nx's");
  last = now;
  last_frames = frames;
}

constexpr uint32_t kDeviceVertexShader = 12696;  // 0x3198
constexpr uint32_t kDevicePixelShader = 12692;   // 0x3194

}  // namespace

// Draw-time shader load (FlushState's shader part, inlined into every Draw*).
REX_EXTERN(__imp__sub_821FAF10);
REX_HOOK_RAW(sub_821FAF10) {
  BANJO_RENUT_YIELD(sub_821FAF10)  // Renut's renderer chosen (banjo_renderer = "renut")
  const uint32_t device = ctx.r3.u32;
  if (device >= 0x40000000 && device < 0x7F000000) {
    me::native::NoteShaderObject(base, Load32(base, device + kDeviceVertexShader), true);
    me::native::NoteShaderObject(base, Load32(base, device + kDevicePixelShader), false);
  }
  if (banjo::d3d::CensusOn()) [[unlikely]] {
    banjo::d3d::CensusEnter(0x821FAF10u, ctx.r3.u32, base);
    __imp__sub_821FAF10(ctx, base);
    banjo::d3d::CensusLeave(base);
    return;
  }
  __imp__sub_821FAF10(ctx, base);
}

REX_EXTERN(__imp__sub_821FB880);
REX_HOOK_RAW(sub_821FB880) {  // D3DDevice_DrawVertices
  BANJO_RENUT_YIELD_SHARED(sub_821FB880)  // Renut's renderer chosen (banjo_renderer = "renut")
  if (banjo::d3d::CensusOn()) [[unlikely]] {  // banjo_diag_d3d_census: counted, the original unchanged
    banjo::d3d::NoteDraw(base, ctx.r3.u32, 1, ctx.r6.u32);
    banjo::d3d::CensusEnter(0x821FB880u, ctx.r3.u32, base);
    __imp__sub_821FB880(ctx, base);
    banjo::d3d::CensusLeave(base);
    return;
  }
  uint32_t& depth = ctx.host_counters[me::native::kRecordDepthSlot];
  auto direct = me::native::BeginDrawDirect(base, ctx.r3.u32, depth, 0x821FB880u);
  me::native::NoteDrawCall(base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  // banjo_d3d_native_draw_vertices; banjo_d3d_records_native: the record's operations written as the packets are
  if (!banjo::d3d::NativeDraw<false>(ctx, base, direct.ops.begin ? &direct.ops : nullptr)) {
    direct.ops.cur = nullptr;
    __imp__sub_821FB880(ctx, base);
  }
  me::native::EndDrawDirect(base, direct, depth);
}

REX_EXTERN(__imp__sub_8263F358);
REX_HOOK_RAW(sub_8263F358) {  // D3DDevice_DrawIndexedVertices
  BANJO_RENUT_YIELD_SHARED(sub_8263F358)  // Renut's renderer chosen (banjo_renderer = "renut")
  if (banjo::d3d::CensusOn()) [[unlikely]] {  // banjo_diag_d3d_census: counted, the original unchanged
    banjo::d3d::NoteDraw(base, ctx.r3.u32, 0, ctx.r7.u32);
    banjo::d3d::CensusEnter(0x8263F358u, ctx.r3.u32, base);
    __imp__sub_8263F358(ctx, base);
    banjo::d3d::CensusLeave(base);
    return;
  }
  uint32_t& depth = ctx.host_counters[me::native::kRecordDepthSlot];
  auto direct = me::native::BeginDrawDirect(base, ctx.r3.u32, depth, 0x8263F358u);
  me::native::NoteDrawCall(base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  // banjo_d3d_native_draw_indexed; banjo_d3d_records_native: the record's operations written as the packets are
  if (!banjo::d3d::NativeDraw<true>(ctx, base, direct.ops.begin ? &direct.ops : nullptr)) {
    direct.ops.cur = nullptr;
    __imp__sub_8263F358(ctx, base);
  }
  me::native::EndDrawDirect(base, direct, depth);
}

REX_EXTERN(__imp__sub_821FD698);
REX_HOOK_RAW(sub_821FD698) {  // D3DDevice_DrawVerticesUP
  BANJO_RENUT_YIELD_SHARED(sub_821FD698)  // Renut's renderer chosen (banjo_renderer = "renut")
  if (banjo::d3d::CensusOn()) [[unlikely]] {  // banjo_diag_d3d_census: counted, the original unchanged
    banjo::d3d::NoteDraw(base, ctx.r3.u32, 2, ctx.r5.u32);
    banjo::d3d::CensusEnter(0x821FD698u, ctx.r3.u32, base);
    __imp__sub_821FD698(ctx, base);
    banjo::d3d::CensusLeave(base);
    return;
  }
  uint32_t& depth = ctx.host_counters[me::native::kRecordDepthSlot];
  auto direct = me::native::BeginDrawDirect(base, ctx.r3.u32, depth, 0x821FD698u);
  direct.ops = {};  // (no native path: its records are decoded)
  me::native::NoteDrawCall(base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  __imp__sub_821FD698(ctx, base);
  me::native::EndDrawDirect(base, direct, depth);
}

// Direct3D's one-rectangle clear (its only caller is D3DDevice_Clear's per-rectangle step sub_8224F110): markers before
// and after its packets for the renderer (banjo_d3d_api_clear, me/banjo_d3d_clear.h).
REX_EXTERN(__imp__sub_821F2750);
REX_HOOK_RAW(sub_821F2750) {
  BANJO_RENUT_YIELD(sub_821F2750)  // Renut's renderer chosen (banjo_renderer = "renut")
  const uint32_t device = ctx.r3.u32;
  const bool marked = banjo::d3d::clear::Begin(ctx, base);
  __imp__sub_821F2750(ctx, base);
  if (marked) banjo::d3d::clear::End(device, base);
}

// D3DDevice_Resolve: markers before and after its packets (banjo_d3d_api_resolve, me/banjo_d3d_clear.h).
REX_EXTERN(__imp__sub_821FC7A0);
REX_HOOK_RAW(sub_821FC7A0) {
  BANJO_RENUT_YIELD_SHARED(sub_821FC7A0)  // Renut's renderer chosen (banjo_renderer = "renut")
  const uint32_t device = ctx.r3.u32;
  uint32_t& depth = ctx.host_counters[me::native::kRecordDepthSlot];
  const auto record = me::native::BeginWriterDirect(base, device, depth, 0x821FC7A0u);  // banjo_d3d_records
  const bool marked = banjo::d3d::clear::ResolveBegin(ctx, base);
  __imp__sub_821FC7A0(ctx, base);
  if (marked) banjo::d3d::clear::ResolveEnd(device, base);
  me::native::EndDrawDirect(base, record, depth);
}

// D3DVertexBuffer_Lock / D3DIndexBuffer_Lock (r3 = the buffer): the buffer's pages are noted with a new Lock sequence
// number before the game gets the pointer (me/banjo_d3d_locks.h). The whole buffer from its header (+24 the address,
// +28 its size word), whatever part is locked: a superset.
REX_EXTERN(__imp__sub_8263D5E8);
REX_HOOK_RAW(sub_8263D5E8) {  // D3DVertexBuffer_Lock
  BANJO_RENUT_YIELD_SHARED(sub_8263D5E8)  // Renut's renderer chosen (banjo_renderer = "renut")
  const uint32_t vb = ctx.r3.u32;
  if (vb >= 0x10000u && vb < 0xFFFFFF00u) {
    const uint8_t* h = base + vb + (vb >= 0xE0000000u ? ::banjo::hot::PhysOff(vb) : 0u);
    banjo::d3d::locks::Mark(Load32(h, 24) & ~3u, Load32(h, 28) & 0x03FFFFFCu);
  }
  __imp__sub_8263D5E8(ctx, base);
}
REX_EXTERN(__imp__sub_8263D6F8);
REX_HOOK_RAW(sub_8263D6F8) {  // D3DIndexBuffer_Lock
  BANJO_RENUT_YIELD_SHARED(sub_8263D6F8)  // Renut's renderer chosen (banjo_renderer = "renut")
  const uint32_t ib = ctx.r3.u32;
  if (ib >= 0x10000u && ib < 0xFFFFFF00u) {
    const uint8_t* h = base + ib + (ib >= 0xE0000000u ? ::banjo::hot::PhysOff(ib) : 0u);
    banjo::d3d::locks::Mark(Load32(h, 24), std::min<uint32_t>(Load32(h, 28) & 0x03FFFFFFu, 64u << 20));
  }
  __imp__sub_8263D6F8(ctx, base);
}

REX_EXTERN(__imp__sub_822554F8);
REX_HOOK_RAW(sub_822554F8) {  // D3DDevice_Swap
  if (::banjo::renut_gpu::Active()) {  // Renut's renderer chosen (banjo_renderer = "renut")
    renut_native_sub_822554F8(ctx, base);
    g_banjo_game_frames.fetch_add(1, std::memory_order_relaxed);  // the captures' video and the frame rate
    static auto last_report = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last_report >= std::chrono::seconds(10)) {
      last_report = now;
      ReportFrameRate(now);
    }
    return;
  }
  if (banjo::d3d::CensusOn()) [[unlikely]] {
    banjo::d3d::CensusEnter(0x822554F8u, ctx.r3.u32, base);
    __imp__sub_822554F8(ctx, base);
    banjo::d3d::CensusLeave(base);
    g_banjo_game_frames.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  me::native::NoteMainDevice(ctx.r3.u32);  // banjo_d3d_records: the writers' device when their r3 is not one
  uint32_t& depth = ctx.host_counters[me::native::kRecordDepthSlot];
  const auto record = me::native::BeginWriterDirect(base, ctx.r3.u32, depth, 0x822554F8u);
  __imp__sub_822554F8(ctx, base);
  me::native::EndDrawDirect(base, record, depth);
  g_banjo_game_frames.fetch_add(1, std::memory_order_relaxed);
  // The native draw path's counters, every 10 s (one clock read per Swap).
  static auto last_report = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (now - last_report >= std::chrono::seconds(10)) {
    last_report = now;
    banjo::d3d::NativeReport();
    banjo::d3d::clear::Report();
    ReportFrameRate(now);
  }
}

// banjo_d3d_records: Direct3D's space function (the kick and the next segment; its "more space" sub_821B28B0 calls it),
// which gives a call that runs out of room a new segment: the call's records are then made of what it writes there.
REX_EXTERN(__imp__sub_821DFFF0);
REX_HOOK_RAW(sub_821DFFF0) {
  BANJO_RENUT_YIELD_SHARED(sub_821DFFF0)  // Renut's renderer chosen (banjo_renderer = "renut")
  const uint32_t device = ctx.r3.u32;
  const uint32_t old_end = device >= 0x40000000u && device < 0x7F000000u ? Load32(base, device + 48) : 0;
  __imp__sub_821DFFF0(ctx, base);
  me::native::NoteSegmentSwitch(base, device, ctx.host_counters[me::native::kRecordDepthSlot], old_end);
}

// Direct3D's kick (one indirect buffer per segment into the primary ring, then the write pointer): banjo_d3d_host_submit
// (step 6 of docs/d3d-full-replacement.md) hands the segments to the decoder through a host queue.
REX_EXTERN(__imp__sub_821DF948);
REX_HOOK_RAW(sub_821DF948) {
  BANJO_RENUT_YIELD(sub_821DF948)  // Renut's renderer chosen (banjo_renderer = "renut")
  const bool noted = me::native::KickBegin(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  __imp__sub_821DF948(ctx, base);
  if (noted) me::native::KickEnd();
}
// Direct3D's copy of packets of its own into the primary ring (the next segment's tile select), which the next kick
// submits: queued as operations too.
REX_EXTERN(__imp__sub_8263FD60);
REX_HOOK_RAW(sub_8263FD60) {
  BANJO_RENUT_YIELD(sub_8263FD60)  // Renut's renderer chosen (banjo_renderer = "renut")
  me::native::RingWords(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  __imp__sub_8263FD60(ctx, base);
}

// banjo_d3d_records: the hooks on Direct3D's other command-buffer writers (tools/d3d/d3d_record_hooks.py writes the
// file). The census counts through them when it is on (they replace its own hooks on these functions).
#define BANJO_D3D_RECORD_HOOK(ADDR) BANJO_D3D_RECORD_HOOK_WITH(ADDR, BANJO_RENUT_YIELD(sub_##ADDR))
// (banjo_renut_shared.txt: an address Renut's renderer hooks too, tools/renut_gpu_import.py)
#define BANJO_D3D_RECORD_HOOK_SHARED(ADDR) BANJO_D3D_RECORD_HOOK_WITH(ADDR, BANJO_RENUT_YIELD_SHARED(sub_##ADDR))
#define BANJO_D3D_RECORD_HOOK_WITH(ADDR, YIELD)                                             \
  REX_EXTERN(__imp__sub_##ADDR);                                                            \
  REX_HOOK_RAW(sub_##ADDR) {                                                                \
    YIELD                                                                                   \
    if (banjo::d3d::CensusOn()) [[unlikely]] {                                              \
      banjo::d3d::CensusEnter(0x##ADDR##u, ctx.r3.u32, base);                               \
      __imp__sub_##ADDR(ctx, base);                                                         \
      banjo::d3d::CensusLeave(base);                                                        \
      return;                                                                               \
    }                                                                                       \
    uint32_t& depth = ctx.host_counters[me::native::kRecordDepthSlot];                     \
    const auto record = me::native::BeginWriterDirect(base, ctx.r3.u32, depth, 0x##ADDR##u); \
    __imp__sub_##ADDR(ctx, base);                                                           \
    me::native::EndDrawDirect(base, record, depth);                                         \
  }
#if __has_include("me/banjo_d3d_record_hooks.inc")
#include "me/banjo_d3d_record_hooks.inc"
#endif

// The census build's hooks (tools/d3d/d3d_census_build.py writes the file; a normal build has none).
#if __has_include("me/census/banjo_d3d_census_hooks.inc")
#include "me/census/banjo_d3d_census_hooks.inc"
#endif
