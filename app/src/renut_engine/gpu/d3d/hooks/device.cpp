/**
 * @file    gpu/d3d/hooks/device.cpp
 * @brief   Device lifecycle hooks: creation and release, the command ring,
 *          fences, command buffer recording and Swap.
 *
 * The command ring is a stub, as in reblue: m_pRing points at a scratch block
 * that KickOff simply rewinds, and none of the hooked D3D calls writes packets
 * (d3d_native.h). The renderer consumes D3D's API-level state instead (the
 * device's constant/packet shadows and the hooked state-setting and draw calls).
 *
 * The renderer runs on its own thread (render_thread.h). KickOff and Swap
 * queue the end of a batch of work and its fence, which completes once the
 * render thread has read the batch's data; D3D's fence waits wait for that
 * (BlockOnFence_hook), and Swap waits when the game gets a frame too far ahead.
 */
#include <rex/frame_latency.h>  // banjo-nx (2026-10-19)
#include "renut_engine/gpu/stage_census.h"  // blur-nx (2026-11-17, the Renut audit's census)
#include "renut_engine/gpu/host_batch.h"  // blur-nx (2026-11-18, the Renut host batch)
#include "renut_engine/display_sync.h"
#include "renut_engine/gpu/render/pipeline.h"  // banjo-nx (2026-10-20): SyncPipelineListLocked
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/hook.h>

#include "renut_engine/gpu/banjo_renut_tuning.h"  // banjo-nx: CountFenceQuery
#include "renut_engine/gpu/banjo_renut_checks.h"  // banjo-nx (package 27c): QueueKickFlush
#include "renut_engine/gpu/banjo_renut_select.h"
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/ppc.h>
#include <rex/system/xvideo.h>
#include <rex/types.h>

#include "renut_engine/perf/frame_stats.h"
#include "renut_engine/gpu/guest/callbacks.h"
#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/guest/guest_records.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/memory_report.h"
#include "renut_engine/gpu/backend/queued.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/render/textures.h"
#include "renut_engine/gpu/backend/video.h"

#include <rex/cvar.h>

// banjo-nx (2026-10-20, from sa2-nx): the SDK's Switch core layout (sdk/src/system/xthread.cpp).
extern "C" uint32_t RexSetGuestThreadCoreLayout(int main_core, uint32_t others_mask, int others_preferred);
void renutFrameLimit();  // katamari_keys.cpp: the player's frame_cap
extern std::atomic<uint64_t> g_banjo_game_frames;  // katamari_keys.cpp: game frames swapped (D3DDevice_Swap_hook)

// Port 169 (from fnxf_xiii): see D3DDevice_Swap_hook. Accuracy, so off by default.
REXCVAR_DEFINE_BOOL(banjo_renut_scaler_crop, false, "Banjo/Enhancements",
                    "The scaler's source rect (Swap's D3DVIDEO_SCALER_PARAMETERS, or the device's own copy) picks the "
                    "part of the front buffer scanned out: a game that renders below 720p into a corner of it was shown "
                    "whole. The device's copy is at its XDK's offset (0x3584 here, the base's SetScalerParameters). "
                    "false: the whole front buffer, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Port 161 (from fnxf_xiii, its note of 2026-11-05): see D3DDevice_Swap_hook's present closure.
REXCVAR_DEFINE_BOOL(banjo_renut_bookkeeping_after_present, true, "Banjo/Enhancements",
                    "The render thread's frame bookkeeping (texture and stream-cache frames, the resolution latch, the "
                    "memory report, the idle pipelines and spare buffers) after the frame's present, while the GPU "
                    "draws it (fnxf_xiii: 15-37 ms spikes in it held the submit back and the frame missed its refresh). "
                    "false: before the present, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// banjo-nx (2026-10-20, from sa2-nx's sa2_renut_core_layout): see D3DDevice_Swap_hook.
REXCVAR_DEFINE_BOOL(banjo_renut_core_layout, false, "Banjo/Enhancements",
                    "Renut's renderer on the Switch: at the first Swap the guest main thread (N&B's drawing thread, the "
                    "one that calls Direct3D) is pinned to core 0 and every other guest thread gets cores 1-2 "
                    "preferring 2 (the render thread is on core 1 at a higher priority), so no game thread time-slices "
                    "with it (Horizon slices a core's threads of one priority in 10 ms). sa2-nx: render thread 12.5 -> "
                    "11.5 ms and game thread 17.6 -> 17.3 ms with its other items. OFF by default since package 36q: in "
                    "Ryujinx it hung the first Swap in 4 of 12 runs (RexSetGuestThreadCoreLayout re-masking running threads "
                    "under the object table's lock); configs/banjo_renut_core_layout_on.toml to try it on the console")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

using namespace renut::gpu;

// Kernel exports the device code uses.
// katamari-nx: Beautiful Katamari's GetCurrentThreadId is sub_824C1C10 (`lwz r11,256(r13); lwz r3,332(r11); blr`,
// N&B's rex_GetCurrentThreadId 0x8220F1E0 byte for byte).
REX_IMPORT(__imp__sub_824C1C10, GuestGetCurrentThreadId, u32());
REX_IMPORT(__imp__RtlInitializeCriticalSection, RtlInitializeCriticalSection, void(u32));
REX_IMPORT(__imp__KeQueryPerformanceFrequency, KeQueryPerformanceFrequency, u32());
REX_IMPORT(__imp__KeGetCurrentProcessType, KeGetCurrentProcessType, u32());
REX_IMPORT(__imp__VdGetCurrentDisplayGamma, VdGetCurrentDisplayGamma, void(u32, u32));

namespace {

bool g_gamma_piecewise = false;  // halo3-nx (2026-11-03): which form the device's gamma ramp is in (Video::NoteGameGamma)

// Scratch space packets go to: the inline writers in N&B's own code (D3D's
// header helpers), which reserve against m_pRingGuarantee. Large enough for
// the biggest single reservation; the guarantee margin is what inline writers
// assume is free without calling KickOff.
constexpr u32 kRingSinkSize = 4u << 20;
constexpr u32 kRingSinkGuaranteeMargin = 256u << 10;
// Scratch handed out for inline data (RingBufferDeviceAllocate), reused in a
// circle: nothing reads it either.
constexpr u32 kRingDataSize = 1u << 20;

// CDevice-private state Direct3D_CreateDevice (0x82634990) sets up, as byte
// offsets into the device allocation.
// Byte whose 0x80 bit marks a device without GPU hardware (its device-type-2
// path sets it); D3D's code skips hardware work for it.
constexpr u32 kDeviceOffsetFlags = 10940;
constexpr u8 kDeviceFlagNoHardware = 0x80;
// Byte whose 0x2 bit makes CDevice::SetFence (0x82246420) write each fence's
// completion value itself instead of waiting for the GPU to. The title device
// runs without it once created: its fences complete on the render thread.
constexpr u32 kDeviceOffsetFlags2 = 10941;
constexpr u8 kDeviceFlagImmediateFences = 0x02;
// Pointer to the completed-fence word IsFencePending / BlockOnFence read.
constexpr u32 kDeviceOffsetFenceCompletion = 10896;
// Nonzero while a KickOff is in progress (BlockOnFence does not kick then).
constexpr u32 kDeviceOffsetInKickOff = 13216;  // katamari-nx: 13232->13216
// Side buffer (0x12C0 bytes) every device gets.
constexpr u32 kDeviceOffsetSideBuffer = 16704;  // katamari-nx: 16728->16704
constexpr u32 kSideBufferSize = 0x12C0;
// Set to -1 by the device-type-2 path.
constexpr u32 kDeviceOffsetsMinusOne[] = {21532, 21536, 14912};  // katamari-nx: 21560->21532, 21564->21536, 14936->14912
constexpr u32 kDeviceTypeNoHardware = 2;
// Copied from the global qword at 0x82DA3478 into every device.
constexpr u32 kDeviceOffsetGlobalQword = 10880;
constexpr u32 kGlobalQwordVa = 0x82AD93F8;  // katamari-nx: 0x82DA3478->0x82AD93F8
// Hardware-path fields: two critical sections, QueryPerformanceFrequency and
// its reciprocal as floats, and 41 words set to -1.
constexpr u32 kDeviceOffsetCriticalSection0 = 14920;  // katamari-nx: 14944->14920
constexpr u32 kDeviceOffsetCriticalSection1 = 14948;  // katamari-nx: 14972->14948
constexpr u32 kDeviceOffsetPerfFrequency = 21560;  // katamari-nx: 21588->21560
constexpr u32 kDeviceOffsetTitleMinusOne = 21672;  // katamari-nx: 23380->21672
// Import slot holding the address of the kernel's VdGlobalDevice variable.
constexpr u32 kVdGlobalDeviceImportSlot = 0x82000750;  // katamari-nx: 0x820007F8->0x82000750

constexpr u32 kE_OUTOFMEMORY = 0x8007000Eu;

u32 g_ring_sink = 0;
u32 g_ring_data = 0;
std::atomic<u32> g_ring_data_offset{0};

// Device blocks of released devices, for the next Direct3D_CreateDevice (the
// records heap never frees physical blocks). N&B creates and releases a pair
// of no-hardware devices around every section it loads.
std::mutex g_free_devices_mutex;
std::vector<u32> g_free_devices;

void WriteU32(u8 *device_bytes, u32 offset, u32 value) {
  *reinterpret_cast<be_u32 *>(device_bytes + offset) = value;
}

u32 LoadGuestU32(u32 va) {
  const auto *p = GuestPtr<const be_u32>(va);
  return p ? u32(*p) : 0;
}

// The newest fence queued for completion (the title device's).
std::atomic<u32> g_queued_fence{0};

// What CDevice::SetFence (0x82246420) does at a KickOff: the current fence
// covers the work issued so far, and the device moves on to the next. The
// fence completes when the render thread has read that work's data (and
// written its memexport results): until then the game must not reuse it.
// Without completing no resource tagged with a fence would stop being busy.
void KickFence(u8 *bytes) {
  const u32 fence = *reinterpret_cast<const be_u32 *>(bytes + d3d::kDeviceOffsetFenceTag);
  const u32 completion = *reinterpret_cast<const be_u32 *>(bytes + kDeviceOffsetFenceCompletion);
  WriteU32(bytes, d3d::kDeviceOffsetFenceTag, fence + 2);
  g_queued_fence.store(fence, std::memory_order_release);
  render_thread::QueueFenceCompletion(completion, fence, LastCallback());
}

// IsFencePending's test.
bool FencePending(const u8 *bytes, u32 fence) {
  const u32 tag = *reinterpret_cast<const be_u32 *>(bytes + d3d::kDeviceOffsetFenceTag);
  const u32 completion = *reinterpret_cast<const be_u32 *>(bytes + kDeviceOffsetFenceCompletion);
  const auto *word = GuestPtr<const be_u32>(completion);
  return word && tag - fence < tag - u32(*word);
}

// ---------------------------------------------------------------------------
// Device initialisation (N&B's D3D, natively)
// ---------------------------------------------------------------------------

// D3D::XenonGpu_Initialize: the register image's power-on values, everything
// dirty. (Its register writes go into the ring.)
void XenonGpuInitialize(native::Device d) {
  d.SetQ(0x2E28, 0x00000000FFFFFFFFull);  // katamari-nx: 0x2E38->0x2E28
  for (u32 i = 0; i < 26; ++i)  // texture fetch constants: type 0
    d.SetW(native::off::TextureFetch + i * 24, d.W(native::off::TextureFetch + i * 24) & ~3u);
  for (u32 i = 0; i < 18; ++i) {  // vertex fetch constants: type 1
    const u32 offset = native::off::VertexFetch + i * 8;
    d.SetW(offset, (d.W(offset) & ~3u) | 1);
  }
  d.SetW(0x28BC, 0x20002000);
  d.SetW(0x29D4, 0xFF000);
  d.SetW(0x2A14, 0x10);
  d.SetW(native::off::ClipControl, d.W(native::off::ClipControl) | 0x80000);
  d.SetW(0x2984, 0xE);
  d.SetW(0x2A10, 0xE);
  d.SetW(0x296C, 8);
  d.SetW(0x29D8, 0xFF100);
  d.SetW(0x2954, 4);
  // PA_SU_SC_MODE_CNTL vtx_window_offset_enable: one-tile draws are in screen
  // space (see draw.cpp, ComputeViewport).
  d.SetW(native::off::ModeControl, d.W(native::off::ModeControl) | 0x10000);
  d.SetW(0x28CC, 0xFFFFFF);
  d.SetW(0x29C0, 4);
  d.SetW(0x2A48, 2);
  for (u32 word = 0; word < 5; ++word)
    d.SetPending(word, ~u64(0));
  d.SetW(0x2AA4, 0xE);
}

// D3D::InitializeApiState (0x82641BC8).
void InitializeApiState(native::Device d) {
  const u32 present_interval = d.W(0x3504);  // katamari-nx: 0x351C->0x3504
  const u32 behavior_flags2 = d.W(0x56F8);  // katamari-nx: 0x5DAC->0x56F8
  native::InitializeStateTables(d);
  d.SetW(0x2F98, 5);  // katamari-nx: 0x2FA8->0x2F98
  d.SetW(0x2F9C, 1);  // katamari-nx: 0x2FAC->0x2F9C
  native::SetScreenExtentQueryMode(d, 2);  // D3DSEQM_PRECLIP
  d.SetW(0x3088, 2);  // katamari-nx: 0x3098->0x3088
  d.SetW(0x2FD0, 0x2A23B9);  // katamari-nx: 0x2FE0->0x2FD0
  d.SetH(0x2FCC, 0);  // katamari-nx: 0x2FDC->0x2FCC
  d.SetH(0x2FCE, 0);  // katamari-nx: 0x2FDE->0x2FCE
  d.SetB(0x2FD4, 0);  // katamari-nx: 0x2FE4->0x2FD4
  d.SetB(0x2FD5, 0);  // katamari-nx: 0x2FE5->0x2FD5
  d.SetB(0x2FD6, 0);  // katamari-nx: 0x2FE6->0x2FD6
  d.SetW(0x2FDC, ~0u);  // katamari-nx: 0x2FEC->0x2FDC
  // Three zero bytes; N&B leaves the fourth from its stack.
  d.SetW(0x2FE0, 0);  // katamari-nx: 0x2FF0->0x2FE0
  d.SetW(0x2FD8, 0x00FF0000);  // katamari-nx: 0x2FE8->0x2FD8
  d.SetW(0x2FB0, 1);  // katamari-nx: 0x2FC0->0x2FB0
  d.SetW(0x2FB4, 0);  // katamari-nx: 0x2FC4->0x2FB4
  d.SetW(0x2FC8, 0);  // katamari-nx: 0x2FD8->0x2FC8
  // The default vertex declaration is the device's own, at +0x2FA8.
  d.SetW(d3d::kDeviceOffsetVertexDeclaration, d.va + 0x2F98);  // katamari-nx: 0x2FA8->0x2F98
  d.Dirty(2, 0x80000);
  d.SetW(0x3504, present_interval);  // katamari-nx: 0x351C->0x3504
  const u32 flags2 = d.W(0x56F8);  // katamari-nx: 0x5DAC->0x56F8
  d.SetW(0x56F8, (behavior_flags2 & 1) ? (flags2 | 1) : (flags2 & ~1u));  // katamari-nx: 0x5DAC->0x56F8
  d.SetW(native::off::PredicationSet, 0);
  native::UpdateTilePredicated(d);
  d.SetW(native::off::Predication, ~0u);
  d.SetW(native::off::RunPredicationState, 0);
  d.SetW(native::off::TilePredication, 0);
  d.SetW(native::off::RunPredication, 0);
  if (!(d.B(native::off::Flags) & native::kFlagsNoHardware))
    native::SetShaderGPRAllocation(d, 0, 0, 0);
}

// sub_82644AC8: the scaler parameters (D3DPRESENT_PARAMETERS +0x60) the
// device keeps. N&B's code then derives the default scaling for the video
// mode and passes it to VdCallGraphicsNotificationRoutines: the display's.
void SetScalerParameters(native::Device d, const be_u32 *scaler) {
  u32 copy[14] = {};
  for (u32 i = 0; i < 6; ++i)
    copy[i] = scaler[i];
  copy[6] = copy[10] = scaler[6] ? 3 : 0;
  for (u32 i = 0; i < 14; ++i)
    d.SetW(0x3584 + i * 4, copy[i]);  // katamari-nx: 0x359C->0x3584
  d.SetB(native::off::Flags2, d.B(native::off::Flags2) | 0x10);
}

void SetGammaRamp(native::Device d, const u16 *ramp);
void SetPWLGamma(native::Device d, const u16 *pwl);

// BOOL D3D::InitializePresentationParameters(CDevice*, D3DPRESENT_PARAMETERS*)
// [0x82641EA8]: the video mode, the automatic back/depth buffers and the
// presentation state. N&B's parameters (sub_823BB980) disable the automatic
// front buffer and depth buffer and ask for the back buffer.
bool InitializePresentationParameters(native::Device d, u32 parameters_va) {
  const auto *p = GuestPtr<const be_u32>(parameters_va);
  if (!p)
    return false;
  const u32 width = p[0];
  const u32 height = p[1];
  const u32 back_format = p[2];
  const u32 multisample = p[4];
  const u32 depth_format = p[10];
  u32 front_format = p[16];
  // The front buffer format, as the scanout reads it.
  if (front_format == 0 || front_format == 0x28280186u)
    front_format = 0x28280106u;
  else if (front_format == 0x18280186u)
    front_format = 0x18280106u;
  else if (front_format == 0x282801B6u)
    front_format = 0x28280136u;
  else if (front_format == 0x182801B6u)
    front_format = 0x18280136u;

  rex::system::X_VIDEO_MODE mode{};
  rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
  d.SetW(0x540C, u32(mode.display_width));  // katamari-nx: 0x5428->0x540C
  d.SetW(0x5414, u32(mode.display_width));  // katamari-nx: 0x5430->0x5414
  d.SetW(0x5418, u32(i64(float(mode.refresh_rate) + 0.5f)));  // katamari-nx: 0x5434->0x5418
  d.SetW(0x5410, u32(mode.display_height));  // katamari-nx: 0x542C->0x5410

  if (!p[15])  // DisableAutoFrontBuffer
    RGPU_ERROR("InitializePresentationParameters: an automatic front buffer is not supported");
  if (!p[14]) {  // DisableAutoBackBuffer
    const u32 surface = native::CreateSurface(width, height, back_format, multisample, 0);
    if (!surface)
      return false;
    d.SetW(native::off::AutoBackBuffer, surface);
    native::SetRenderTarget(d, 0, surface);
  }
  if (p[9]) {  // EnableAutoDepthStencil
    const u32 surface = native::CreateSurface(width, height, depth_format, multisample, 0);
    if (!surface)
      return false;
    d.SetW(native::off::AutoDepthStencil, surface);
    native::SetDepthStencilSurface(d, surface);
  }
  for (u32 i = 0; i < 0x7C / 4; ++i)
    d.SetW(0x34D0 + i * 4, p[i]);  // katamari-nx: 0x34E8->0x34D0
  if ((front_format & 0x3FFC0000u) != 0x24900000u)
    d.SetW(0x3510, (front_format & 0x3F) | 0x28280100u);  // katamari-nx: 0x3528->0x3510
  SetScalerParameters(d, p + 0x60 / 4);
  // The default gamma: linear, as a piecewise-linear ramp for the front
  // buffer formats 7 and 0x36 (0x32 counts as 6).
  u32 gamma_format = front_format & 0x3F;
  if (gamma_format == 0x32)
    gamma_format = 6;
  else if (gamma_format == 7)
    gamma_format = 0x36;
  u16 ramp[768];
  if (gamma_format == 7 || gamma_format == 0x36) {
    for (u32 k = 0; k < 128; ++k) {
      const u16 base = u16((0xFFFFu * k) / 0x7F);
      const u16 delta = u16((0xFFFFu * (k + 1)) / 0x7F - base);
      for (u32 c = 0; c < 3; ++c) {
        ramp[c * 256 + k * 2] = base;
        ramp[c * 256 + k * 2 + 1] = delta;
      }
    }
    SetPWLGamma(d, ramp);
  } else {
    for (u32 i = 0; i < 256; ++i) {
      const u16 value = u16((((0x3FFu * i) / 0xFF) & 0x3FF) << 6);
      ramp[i] = ramp[256 + i] = ramp[512 + i] = value;
    }
    SetGammaRamp(d, ramp);
  }
  d.SetW(0x412C, ~0u);  // katamari-nx: 0x4144->0x412C
  return true;
}

// The hardware path (D3D__InitializeHardwareDevice 0x82642300) minus the GPU:
// the device becomes the one D3D internals find through VdGlobalDevice, and
// gets the presentation state and per-device fields they read.
bool InitializeTitleDevice(native::Device d, u32 present_parameters) {
  RtlInitializeCriticalSection(d.va + kDeviceOffsetCriticalSection0);
  RtlInitializeCriticalSection(d.va + kDeviceOffsetCriticalSection1);
  d.SetW(16688, 0);  // katamari-nx: 16712->16688

  // D3D reads the device through the kernel's VdGlobalDevice from any thread
  // (resource locks, blockers).
  if (const u32 global = LoadGuestU32(kVdGlobalDeviceImportSlot))
    *GuestPtr<be_u32>(global) = d.va;

  const float frequency = float(KeQueryPerformanceFrequency());
  const float period = frequency > 0.0f ? 1.0f / frequency : 0.0f;
  d.SetF(kDeviceOffsetPerfFrequency, frequency);
  d.SetF(kDeviceOffsetPerfFrequency + 4, period);
  for (u32 i = 0; i < 41; ++i)
    d.SetW(kDeviceOffsetTitleMinusOne + i * 4, 0xFFFFFFFFu);

  return !present_parameters || InitializePresentationParameters(d, present_parameters);
}

// D3D's CDevice teardown (0x82642568) for our devices: they carry the
// no-hardware flag, which skips everything but unbinding. The side buffer is
// part of our device block, and N&B never gives a device a query block.
void UninitializeDevice(native::Device d) {
  if (d.W(native::off::Ring))
    native::UnsetAll(d);
}

// HRESULT Direct3D_CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, void *pUnused,
//     DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentParameters,
//     IDirect3DDevice9 **ppReturnedDeviceInterface)   [0x82634990]
// N&B creates two: type 1 (the title's rendering device) and type 2 (D3D's
// own no-hardware device, which N&B keeps alongside).
u32 Direct3D_CreateDevice_hook(u32 /*adapter*/, u32 device_type, u32 /*unused*/,
                               u32 behavior_flags, u32 present_parameters,
                               mapped_u32 out_device) {
  if (out_device)
    *out_device = 0u;

  // Every D3D object the renderer creates from here on lives in this heap.
  if (!records::Init())
    return kE_OUTOFMEMORY;

  // The device, its side buffer and fence word share one block of our own
  // guest memory (64 KiB aligned, so each part keeps its 0x80 alignment).
  static_assert(d3d::kDeviceAllocSize % d3d::kDeviceAlignment == 0);
  constexpr u32 kFenceOffset = d3d::kDeviceAllocSize + ((kSideBufferSize + 0x7F) & ~0x7Fu);
  constexpr u32 kBlockSize = kFenceOffset + 0x80;
  u32 block_va = 0;
  {
    std::lock_guard lock(g_free_devices_mutex);
    if (!g_free_devices.empty()) {
      block_va = g_free_devices.back();
      g_free_devices.pop_back();
    }
  }
  if (!block_va)
    block_va = records::AllocPhysicalBlock(kBlockSize);
  const u32 device_va = block_va;
  const u32 side_buffer_va = block_va ? device_va + d3d::kDeviceAllocSize : 0;
  const u32 fence_va = block_va ? device_va + kFenceOffset : 0;
  if (!g_ring_sink)
    g_ring_sink = records::AllocPhysicalBlock(kRingSinkSize);
  if (!g_ring_data)
    g_ring_data = records::AllocPhysicalBlock(kRingDataSize);
  if (!device_va || !g_ring_sink || !g_ring_data) {
    RGPU_ERROR("Direct3D_CreateDevice: guest allocation failed");
    return kE_OUTOFMEMORY;
  }
  std::memset(GuestPtr<u8>(device_va), 0, kBlockSize);
  std::memset(GuestPtr<u8>(g_ring_sink), 0, kRingSinkSize);

  const native::Device d = native::DeviceAt(device_va);
  auto *device = GuestPtr<d3d::Device>(device_va);
  auto *bytes = d.bytes;
  const bool title_device = device_type != kDeviceTypeNoHardware;
  device->m_ReferenceCount = 1u;
  // Fields every device gets.
  WriteU32(bytes, kDeviceOffsetGlobalQword, LoadGuestU32(kGlobalQwordVa));
  WriteU32(bytes, kDeviceOffsetGlobalQword + 4, LoadGuestU32(kGlobalQwordVa + 4));
  WriteU32(bytes, d3d::kDeviceOffsetCreationThreadId, GuestGetCurrentThreadId());
  WriteU32(bytes, d3d::kDeviceOffsetIsTitleProcess, 0);  // KeGetCurrentProcessType() != 2
  WriteU32(bytes, kDeviceOffsetSideBuffer, side_buffer_va);
  WriteU32(bytes, kDeviceOffsetFenceCompletion, fence_va);
  if (title_device) {
    // The original's default when no ring configuration is requested.
    if (!(behavior_flags & 0x100) && !(behavior_flags & 0x3F000000))
      behavior_flags |= 0xC000000;
    WriteU32(bytes, d3d::kDeviceOffsetBehaviorFlags, behavior_flags);
    WriteU32(bytes, 21532, 0xFFFFFFFFu);  // katamari-nx: 21560->21532
    WriteU32(bytes, 21536, 0xFFFFFFFFu);  // katamari-nx: 21564->21536
  } else {
    for (u32 offset : kDeviceOffsetsMinusOne)
      WriteU32(bytes, offset, 0xFFFFFFFFu);
  }
  // Both run without GPU hardware: no hardware setup in the API state, and
  // fences complete as they are written.
  bytes[kDeviceOffsetFlags] |= kDeviceFlagNoHardware;
  bytes[kDeviceOffsetFlags2] |= kDeviceFlagImmediateFences;
  native::RewindRing(d);

  XenonGpuInitialize(d);
  if (title_device && !InitializeTitleDevice(d, present_parameters)) {
    RGPU_ERROR("Direct3D_CreateDevice: InitializePresentationParameters failed");
    return kE_OUTOFMEMORY;
  }
  InitializeApiState(d);
  if (title_device)
    native::ResetAllState(d);
  native::RewindRing(d);
  // From here the title device's fences complete on the render thread
  // (KickFence). The no-hardware device only records command buffers.
  if (title_device)
    bytes[kDeviceOffsetFlags2] &= u8(~kDeviceFlagImmediateFences);
  else
    native::TrackDeviceTouches(device_va);  // banjo-nx (2026-10-26, banjo_renut_tags): a recording device

  if (out_device)
    *out_device = device_va;
  if (title_device)
    Video::SetGuestDevice(device_va);
  RGPU_INFO("Guest D3D device (type {}) created at {:#010x}", device_type, device_va);
  return 0;  // S_OK
}

// ULONG D3DDevice_Release(D3DDevice*) [0x82634AE8]
// As the original, but on the last release the device block goes back to our
// free list instead of N&B's heap (the original frees the side buffer and the
// raw allocation Direct3D_CreateDevice kept at device - 4).
u32 D3DDevice_Release_hook(u32 device_va) {
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return 0;
  const u32 count = device->m_ReferenceCount;
  if (count != 1) {
    device->m_ReferenceCount = count - 1;
    return count - 1;
  }
  FlushDeferredVertices(device_va);
  UninitializeDevice(native::DeviceAt(device_va));
  device->m_ReferenceCount = 0u;
  RGPU_INFO("Guest D3D device at {:#010x} released", device_va);
  // The title device stays the renderer's until another replaces it.
  if (device_va != Video::GuestDevice()) {
    std::lock_guard lock(g_free_devices_mutex);
    g_free_devices.push_back(device_va);
  }
  return 0;
}

// The GPU starts on the kicked draws: the guest buffers they read are read
// then (FlushVertexFillsLocked), before the fence tells the game it may reuse
// them. Memexport results reach guest memory later, once the GPU has finished
// the frame (detail::CompleteMemExportsLocked); draws that read them sooner
// wait for them (DrawStateLocked).
void KickDevice(d3d::Device *device, u32 device_va) {
  FlushDeferredVertices(device_va);
  // banjo-nx (package 27c): the texture fills too (they were read at the submit, after this fence had let the game
  // rewrite them).
  banjo::renut_gpu::checks::QueueKickFlush();
  KickFence(reinterpret_cast<u8 *>(device));
  native::DeviceAt(device_va).Touch(d3d::kDeviceOffsetFenceTag);  // banjo-nx (2026-10-26, banjo_renut_tags)
}

// PRING CDevice::KickOff() [0x821DFFF0]: hand the ring to the GPU. Nothing
// consumes it here, so rewind it and return the new write pointer. A device
// recording a command buffer (N&B records on worker threads with no-hardware
// devices) would chain its segment into the buffer; its draws are recorded on
// the host (command_buffers.h), so it only rewinds.
u32 KickOff_hook(u32 device_va) {
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return 0;
  if (!IsRecordingCommandBuffer(device_va)) {
    RunReadyCallbacks();
    KickDevice(device, device_va);
  }
  native::RewindRing(native::DeviceAt(device_va));
  return device->m_pRing;
}

// void CDevice::KickOffSegment() [0x821DFC08]
void KickOffSegment_hook(u32 device_va) {
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return;
  if (!IsRecordingCommandBuffer(device_va))
    KickDevice(device, device_va);
  native::RewindRing(native::DeviceAt(device_va));
}

// PRING CDevice::BeginRingBig(DWORD Count) [0x821C7360]: room for Count
// dwords, kicking when the segment is full.
u32 BeginRingBig_hook(u32 device_va, u32 count) {
  const native::Device d = native::DeviceAt(device_va);
  if (!d)
    return 0;
  if (d.W(native::off::Ring) + count * 4 > d.W(native::off::RingLimit))
    native::RewindRing(d);
  return d.W(native::off::Ring);
}

// PRING CDevice::BeginRingManual() [0x82A6DD88]
u32 BeginRingManual_hook(u32 device_va) {
  const native::Device d = native::DeviceAt(device_va);
  return d ? d.W(native::off::Ring) : 0;
}

// Inline data space (vertex data of inline draws, clear rects): scratch that
// nothing reads, handed out in a circle.
u32 RingData(u32 bytes, u32 alignment) {
  if (!g_ring_data || bytes > kRingDataSize / 2)
    return 0;
  if (alignment < 4 || (alignment & (alignment - 1)))
    alignment = 4;
  u32 offset = g_ring_data_offset.fetch_add(bytes + alignment, std::memory_order_relaxed);
  offset %= kRingDataSize / 2;
  offset = (offset + alignment - 1) & ~(alignment - 1);
  return g_ring_data + offset;
}

// void *D3D::RingBufferDeviceAllocate(CDevice*, DWORD Flags, DWORD *pSize,
//     DWORD Alignment) [0x821FC4B8]
u32 RingBufferDeviceAllocate_hook(u32 /*device*/, u32 /*flags*/, mapped_u32 size, u32 alignment) {
  return size ? RingData(u32(*size), alignment) : 0;
}

// void *CDevice inline data allocation (sub_821FC318): from the ring, or from
// the allocator of the command buffer being recorded.
u32 AllocateInlineData_hook(u32 /*device*/, u32 dwords, u32 alignment) {
  return RingData(dwords * 4, alignment);
}

// HRESULT D3DDevice_SetRingBufferParameters(D3DDevice*,
//     const D3DRING_BUFFER_PARAMETERS*) [0x82640090]: would replace the ring
// (and our sink) with a freshly allocated one.
u32 D3DDevice_SetRingBufferParameters_hook(u32 /*device*/, u32 /*parameters*/) {
  return 0;  // S_OK
}

// void CDevice::BlockOnFence(DWORD Fence, CBlocker::Type, BOOL NoKick)
// [0x8225D370]: every fence wait (D3DDevice_BlockOnFence, resource locks and
// destruction, InvalidateConstants). As the original: kick the fence if no
// KickOff has covered it yet, then wait for it, here for the render thread to
// complete it (KickFence).
// The original polls through CBlocker::Check, which after 5 s without
// progress dumps the GPU's registers and traps.
u32 BlockOnFence_hook(u32 device_va, u32 fence, u32 /*type*/, u32 /*no_kick*/) {
  banjo::renut_gpu::CountFenceQuery();  // banjo-nx (package 27c): the stream checks' design
  auto *bytes = GuestPtr<u8>(device_va);
  if (!bytes || !FencePending(bytes, fence))
    return 0;
  // Devices with immediate fences (the command buffer recorders) complete
  // theirs as they write them; the original returns at once for them too.
  if (bytes[kDeviceOffsetFlags2] & kDeviceFlagImmediateFences)
    return 0;
  const bool covered =
      int32_t(fence - g_queued_fence.load(std::memory_order_acquire)) <= 0;
  if (!covered) {
    if (*reinterpret_cast<const be_u32 *>(bytes + kDeviceOffsetInKickOff))
      return 0;
    KickOff_hook(device_va);
  }
  // The fence may wait for callbacks, which this thread runs as the render
  // thread reaches them.
  const auto waiting = std::chrono::steady_clock::now();
  const bool hb_census = ::renut::gpu::host_batch::CensusOn();  // blur-nx (2026-11-18, the census's #8)
  const uint64_t hb_t0 = hb_census ? ::renut::gpu::host_batch::Ticks() : 0;
  for (;;) {
    RunReadyCallbacks();
    if (!FencePending(bytes, fence))
      break;
    if (!render_thread::WaitForFenceProgress(std::chrono::milliseconds(2)))
      break;  // no render thread: nothing would complete it
  }
  if (hb_census) {
    ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kFenceWaits);
    ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kFenceWaitTicks, ::renut::gpu::host_batch::Ticks() - hb_t0);
    if (!covered)
      ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kFenceWaitsKicked);
  }
  renut::frame_stats::OnGameThreadWait(
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waiting).count());
  return 0;
}

// BOOL D3DDevice_IsFencePending(DWORD Fence) [0x8225D468]: on the device D3D
// finds through VdGlobalDevice; the current fence is kicked first. Games
// poll it, and the fence may be waiting for callbacks only a guest thread can
// run.
u32 IsFencePending_hook(u32 fence) {
  banjo::renut_gpu::CountFenceQuery();  // banjo-nx (package 27c): the stream checks' design
  RunReadyCallbacks();
  if (!fence)
    return 0;
  const u32 device_va = native::GlobalDevice();
  auto *bytes = GuestPtr<u8>(device_va);
  if (!bytes)
    return 0;
  if (fence == *reinterpret_cast<const be_u32 *>(bytes + d3d::kDeviceOffsetFenceTag))
    KickOff_hook(device_va);
  return FencePending(bytes, fence) ? 1 : 0;
}

// BOOL CBlocker::Check() [0x82634740]: one poll of a D3D wait for the GPU
// (ring space, fence markers, streaming). Fence waits no longer come here
// (BlockOnFence_hook); nothing on the host advances the others.
u32 CBlocker_Check_hook(u32 /*blocker*/) {
  return 0;
}

// ---------------------------------------------------------------------------
// Command buffers
// ---------------------------------------------------------------------------

// D3DCommandBuffer fields.
constexpr u32 kBufferFence = 0x08;
constexpr u32 kBufferInheritTags = 0x18;  // D3DTAGCOLLECTION
constexpr u32 kBufferSetTags = 0x40;      // its copy
constexpr u32 kBufferFlags = 0x6C;        // low nibble: state (2 recording, 1 done, 7 out of memory)
constexpr u32 kBufferFixedMemory = 0x98;  // memory the caller gave it, or 0 for its allocator
constexpr u32 kBufferAllocatorSize = 0xA8;

// D3D::ConfigureForOnePassZPass(CDevice*, BOOL, DWORD TileCount, DWORD Width)
// [0x82641360]: the EDRAM layout of a buffer recorded for a one-pass Z pass.
void ConfigureForOnePassZPass(native::Device d, bool flag, u32 tile_count, u32 width) {
  u8 flags3 = d.B(native::off::Flags3) | 0x20;
  if (flag)
    flags3 |= 0x10;
  d.SetB(native::off::Flags3, flags3);
  const u32 msaa = d.H(native::off::SurfaceInfo) & 3;
  u32 tile_height = 8, tile_width = 32, unit = 80;
  if (msaa == 0) {
    tile_height = 16;
  } else if (msaa != 1) {
    tile_width = 16;
    unit = 40;
  }
  const u32 pitch = (tile_width + width - 1) & ~(tile_width - 1);
  const u32 tiles_per_row = pitch / tile_width;
  for (u32 i = 0; i < tile_count; ++i) {
    const u32 x = d.W(native::off::TileOrigins + i * 8);
    const u32 y = d.W(native::off::TileOrigins + i * 8 + 4);
    d.SetW(0x3330 + i * 4, (y / tile_height) * tiles_per_row + x / tile_width);  // katamari-nx: 0x3340->0x3330
  }
  d.SetW(0x336C, pitch);  // katamari-nx: 0x337C->0x336C
  d.SetW(native::off::SurfaceInfo, native::Insert(d.W(native::off::SurfaceInfo), pitch, 18, 14));
  d.Dirty(2, 0x100);
  d.SetW(0x3370, ((unit + width - 1) / unit) * unit);  // katamari-nx: 0x3380->0x3370
  u64 predicated = d.Q(native::off::PredicatedPending2);
  if ((d.B(native::off::Flags3) & 0x20) || (d.W(0x2E44) & 0xE0000) != 0x40000)  // katamari-nx: 0x2E54->0x2E44
    predicated |= 0x100;
  else
    predicated &= ~u64(0x100);
  d.SetQ(native::off::PredicatedPending2, predicated);
}

// HRESULT D3DDevice_BeginCommandBuffer(D3DDevice*, D3DCommandBuffer*, DWORD Flags,
//     const D3DTAGCOLLECTION *pInheritTags, const D3DRECT *pTilingRects,
//     DWORD TileCount) [0x826291F0] (TU3's build dropped the unused
// pPersistTags before pTilingRects)
// The device records into the buffer: the draws on the host (command_buffers.h),
// so the buffer's segments and the reset of their allocator are left out.
u32 D3DDevice_BeginCommandBuffer_hook(u32 device_va, u32 command_buffer, u32 flags,
                                      u32 inherit_tags, u32 tiling_rects, u32 tile_count) {
  // A Begin*Vertices draw recorded into the previous buffer.
  FlushDeferredVertices(device_va);
  native::Device d = native::DeviceAt(device_va);
  auto *buffer = GuestPtr<be_u32>(command_buffer);
  if (!d || !buffer)
    return 0;
  auto bw = [buffer](u32 offset) -> be_u32 & { return buffer[offset / 4]; };
  bw(kBufferFlags) = native::Insert(bw(kBufferFlags), 2, 0, 4);
  if (const u32 fence = bw(kBufferFence)) {
    ::renut::gpu::census::Scope fence_scope(::renut::gpu::census::kBeginCbFence);  // blur-nx (2026-11-17, audit #22)
    native::BlockOnFence(native::GlobalDevice(), fence);
    bw(kBufferFence) = 0u;
  }
  d.SetW(native::off::CommandBuffer, command_buffer);
  bw(kBufferFlags) = u32(bw(kBufferFlags)) & ~0x70u;
  for (u32 offset : {0x7Cu, 0x80u, 0x84u, 0x70u, 0x90u, 0x74u, 0x78u})
    bw(offset) = 0u;
  d.SetW(0x3A38, 8);  // katamari-nx: 0x3A50->0x3A38
  d.SetB(native::off::Flags1, d.B(native::off::Flags1) & 0xDF);
  d.SetW(0x3A2C, 0);  // katamari-nx: 0x3A44->0x3A2C
  d.SetW(0x3A28, bw(kBufferAllocatorSize));  // katamari-nx: 0x3A40->0x3A28
  for (u32 offset = 0x34B8; offset <= 0x34CC; offset += 4)  // katamari-nx: 0x34D0->0x34B8, 0x34E4->0x34CC
    d.SetW(offset, 0);
  native::RewindRing(d);

  // The tags the buffer inherits (none set unless given) and its copy.
  auto *tags = GuestPtr<const be_u64>(inherit_tags);
  auto *own = reinterpret_cast<be_u64 *>(reinterpret_cast<u8 *>(buffer) + kBufferInheritTags);
  auto *own_copy = reinterpret_cast<be_u64 *>(reinterpret_cast<u8 *>(buffer) + kBufferSetTags);
  for (u32 i = 0; i < 5; ++i) {
    own[i] = tags ? u64(tags[i]) : 0;
    own_copy[i] = u64(own[i]);
  }
  u32 buffer_flags = bw(kBufferFlags);
  buffer_flags &= ~0x80u;
  buffer_flags &= ~0x8000u;
  buffer_flags |= (((flags << 3) & 0x100) | ((flags >> 4) & 1)) << 7;
  if (flags & 8) {
    d.SetW(native::off::CommandBufferTagMask, ~0u);
    buffer_flags |= 0x400;
  } else {
    d.SetW(native::off::CommandBufferTagMask, 0x400000);
    buffer_flags &= ~0x400u;
  }
  bw(0x78) = 0u;
  buffer_flags &= ~0x21800u;
  bw(kBufferFlags) = buffer_flags;
  d.SetQ(native::off::PredicatedPending2, 0);
  d.SetB(native::off::Flags3, d.B(native::off::Flags3) & 0xCF);
  d.SetB(native::off::Flags, d.B(native::off::Flags) & 0x87);

  if (flags & 3) {
    if (flags & 2) {
      bw(kBufferFlags) = u32(bw(kBufferFlags)) | 0x800;
      d.SetB(native::off::Flags, d.B(native::off::Flags) | 8);
    } else {
      // Its ring allocation list (0x33C0) is the GPU's.
      bw(kBufferFlags) = u32(bw(kBufferFlags)) | 0x1000;
    }
    d.SetW(native::off::TileCount, tile_count);
    d.SetB(native::off::Flags, d.B(native::off::Flags) | 0x30);
    u32 extent_x = 0, extent_y = 0;
    const auto *rects = GuestPtr<const be_u32>(tiling_rects);
    for (u32 i = 0; rects && i < tile_count; ++i) {
      const u32 x1 = rects[i * 4], y1 = rects[i * 4 + 1];
      const u32 x2 = rects[i * 4 + 2], y2 = rects[i * 4 + 3];
      if (extent_x <= x2)
        extent_x = x2;
      if (extent_y <= y2)
        extent_y = y2;
      d.SetW(native::off::TileRects + i * 16, x1);
      d.SetW(native::off::TileRects + i * 16 + 4, y1);
      d.SetW(native::off::TileRects + i * 16 + 8, x2);
      d.SetW(native::off::TileRects + i * 16 + 12, y2);
      d.SetW(native::off::TileOrigins + i * 8, x1 & ~31u);
      d.SetW(native::off::TileOrigins + i * 8 + 4, y1 & ~31u);
    }
    d.SetW(native::off::TilingExtent, extent_x);
    d.SetW(native::off::TilingExtent + 4, extent_y);
    d.SetW(0x337C, 0);  // katamari-nx: 0x338C->0x337C
    d.SetW(0x31A8, 2);  // katamari-nx: 0x31B8->0x31A8
    if (flags & 0x40) {
      ConfigureForOnePassZPass(d, (flags >> 7) & 1, tile_count, extent_x);
      bw(kBufferFlags) = u32(bw(kBufferFlags)) | 0x20000;
    }
  }
  if (flags & 4) {
    d.SetB(native::off::Flags, d.B(native::off::Flags) | 0x40);
    d.SetW(0x3198, native::Extract(d.W(native::off::DepthControl), 4, 3));  // katamari-nx: 0x31A8->0x3198
    if (d.B(native::off::Flags3) & 0x20) {
      u64 predicated = d.Q(native::off::PredicatedPending2) | (u64(1) << 37);
      if (d.W(0x3370) != (d.W(native::off::SurfaceInfo) & 0x3FFF))  // katamari-nx: 0x3380->0x3370
        predicated |= u64(1) << 57;
      d.SetQ(native::off::PredicatedPending2, predicated);
    }
  }
  d.SetW(native::off::PredicationSet, 0);
  native::UpdateTilePredicated(d);
  d.SetW(native::off::Predication, ~0u);
  d.SetW(native::off::RunPredicationState, 0);
  d.SetW(native::off::TilePredication, 0);
  d.SetW(native::off::RunPredication, 0);
  // The buffer starts with no fetch constants, and dirty in everything it does
  // not inherit.
  std::memset(d.bytes + native::off::TextureFetch, 0, 0x300);
  d.TouchRange(native::off::TextureFetch, 0x300);  // banjo-nx (2026-10-26, banjo_renut_tags)
  for (u32 i = 0; i < 5; ++i)
    d.SetPending(i, ~u64(own[i]));
  static uint32_t begin_logged = 0;  // halo3-nx (2026-11-03): what this game's recordings inherit
  if (begin_logged < 8) {
    ++begin_logged;
    RGPU_INFO("BeginCommandBuffer {:08X} on device {:08X} (global {:08X}): flags {:X}, inherit tags {:016X} {:016X} {:016X} "
              "{:016X} {:016X}, RT0 {:08X}, depth {:08X}",
              command_buffer, device_va, native::GlobalDevice(), flags, u64(own[0]), u64(own[1]), u64(own[2]), u64(own[3]),
              u64(own[4]), d.W(d3d::kDeviceOffsetRenderTargets), d.W(d3d::kDeviceOffsetDepthStencil));
  }

  BeginRecording(device_va, command_buffer);
  return 0;
}

// HRESULT D3DDevice_EndCommandBuffer(D3DDevice*) [0x82640DE8]
// The state left dirty would be written into the buffer (Flags bit 5 at
// Begin); then everything is unbound, and the device is pointed at its side
// buffer until the next Begin.
u32 D3DDevice_EndCommandBuffer_hook(u32 device_va) {
  FlushDeferredVertices(device_va);
  native::Device d = native::DeviceAt(device_va);
  if (!d)
    return 0;
  auto *buffer = GuestPtr<be_u32>(d.W(native::off::CommandBuffer));
  if (buffer && (u32(buffer[kBufferFlags / 4]) & 0x8000)) {
    d.TouchTags();  // banjo-nx (2026-10-26, banjo_renut_tags)
    for (u32 i = 0; i < 5; ++i)
      d.SetPending(i, 0);
  }
  native::UnsetAll(d);
  // A buffer in fixed memory reports running out of it here; recorded on the
  // host, it does not.
  if (buffer) {
    if (u32(buffer[kBufferFlags / 4]) & 0x1000)
      buffer[0x78 / 4] = 0u;  // its ring allocation list: none
  }
  d.SetW(native::off::CommandBuffer, 0);
  if (buffer) {
    const u32 end = d.W(native::off::Ring) + 4;
    buffer[0x88 / 4] = end;
    buffer[0x8C / 4] = d.W(native::off::RingLimit);
    buffer[0x90 / 4] = native::GpuAddress(end) - 0x40000000u;
  }
  const bool out_of_memory = (d.B(native::off::Flags1) & 0x20) != 0;
  if (buffer)
    buffer[kBufferFlags / 4] = native::Insert(buffer[kBufferFlags / 4], out_of_memory ? 7 : 1, 0, 4);
  // CDevice::MarkAsOutOfMemory: the device writes nowhere until the next Begin.
  native::RewindRing(d);
  d.SetB(native::off::Flags1, d.B(native::off::Flags1) | 0x20);
  return out_of_memory ? kE_OUTOFMEMORY : 0;
}

// void D3DDevice_RunCommandBuffer(D3DDevice*, D3DCommandBuffer*,
//     DWORD PredicationSelect) [0x821F1A78]
// Replaces the original, which would send the buffer's packets to the GPU
// (and on a no-hardware device append to a buffer being recorded).
void D3DDevice_RunCommandBuffer_hook(u32 device_va, u32 command_buffer,
                                     u32 /*predication_select*/) {
  if (!command_buffer)
    return;
  if (IsRecordingCommandBuffer(device_va)) {
    RecordRun(device_va, command_buffer);
  } else {
    RunReadyCallbacks();
    QueueRunCommandBuffer(device_va, command_buffer);
  }
}

// void D3DDevice_Swap(D3DDevice *pDevice, D3DBaseTexture *pFrontBuffer,
//     const D3DVIDEO_SCALER_PARAMETERS *pParameters) [0x822554F8]
// The frame goes to the render thread, which presents it; the game thread
// goes on with the next one, waiting only when it is more than
// render_thread::kMaxFramesAhead ahead.
void D3DDevice_Swap_hook(u32 device_va, u32 front_buffer, u32 parameters) {
  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_core_layout, the Switch): once, the guest main thread alone on core 0,
  // the other guest threads on cores 1-2 (preferring 2: the render thread is pinned to core 1 at a higher priority).
  // Before, every guest thread could run on any core, time-sliced at their priority (0x3B).
  static bool core_layout_done = false;
  if (!core_layout_done) {
    core_layout_done = true;
#if REX_PLATFORM_SWITCH
    if (REXCVAR_GET(banjo_renut_core_layout)) {
      const uint32_t moved = RexSetGuestThreadCoreLayout(0, 0x6, 2);
      RGPU_INFO("[renut] banjo_renut_core_layout: the guest main thread on core 0, the other guest threads on cores "
                "1-2 ({} threads set)", moved);
    }
#endif
  }
  renut::frame_stats::OnGameFrame();
  // blur-nx (2026-11-17): the game frames the captures' video waits on (banjo_video_s) and the D3D census reads, counted
  // here rather than in the Present hook (hooks/draw.cpp, which ends in this hook through native::Swap): Blur swaps
  // without Present, so the count stayed 0 and the video never got its first frame (no file in any Ryujinx run).
  g_banjo_game_frames.fetch_add(1, std::memory_order_relaxed);
  rex::latency::NoteGuestSwap();  // banjo-nx (2026-10-19, input latency): the probe
  const uint64_t swap_ordinal = rex::latency::SwapCount();
  FlushDeferredVertices(device_va);
  SnapshotDeltasNewFrame();  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_snapshot_deltas)

  // What the original does after handing the frame to the video hardware:
  // count the swap, record the frame's fence tag, and end the frame with a
  // KickOff, whose fence completes once the render thread gets there.
  auto *device = GuestPtr<u8>(device_va);
  if (device) {
    auto word = [device](u32 offset) -> be_u32 & {
      return *reinterpret_cast<be_u32 *>(device + offset);
    };
    word(d3d::kDeviceOffsetSwapCount) = u32(word(d3d::kDeviceOffsetSwapCount)) + 1;
    const u32 fence_tag = word(d3d::kDeviceOffsetFenceTag);
    word(d3d::kDeviceOffsetSwapFenceTag) = fence_tag;
    word(d3d::kDeviceOffsetPresentFenceTag) = fence_tag;
    native::DeviceAt(device_va).Touch(d3d::kDeviceOffsetSwapFenceTag);  // banjo-nx (2026-10-26, banjo_renut_tags)
    device[d3d::kDeviceOffsetSwapFlags] &= u8(~d3d::kSwapFlagPending);
    // banjo-nx (package 27c): the frame's last vertex and texture fills are read here, before the Swap's fence
    // completes. They were read at the present, after it: the game, told the frame was done, could rewrite a dynamic
    // buffer for the next frame first, and the frame's last draws (the composite, the HUD) took the new bytes.
    banjo::renut_gpu::checks::QueueKickFlush();
    KickFence(device);
    // banjo-nx (2026-10-20, banjo_renut_game_gamma): the device's gamma ramp (StoreGamma's +0x3A9C) for the present.
    Video::NoteGameGamma(device + 0x3A84, g_gamma_piecewise);  // katamari-nx: 0x3A9C->0x3A84; halo3-nx: the form
  }

  // Port 161 (from fnxf_xiii, its note of 2026-11-05; banjo_renut_bookkeeping_after_present): the frame's bookkeeping
  // (texture and stream-cache frames, the resolution latch, the memory report, the idle pipelines and spare buffers)
  // after the present: the frame goes to the GPU and the display first, and the bookkeeping runs while the GPU draws
  // it (fnxf_xiii's vsync timing: 15-37 ms spikes in it held the submit back, and the frame missed its refresh). It
  // still comes before the next frame's commands (one render thread).
  static const bool after = REXCVAR_GET(banjo_renut_bookkeeping_after_present);
  // Port 169 (banjo_renut_scaler_crop): the scaler's source rect, given (D3DVIDEO_SCALER_PARAMETERS: a D3DRECT first) or
  // the device's own (SetScalerParameters' copy at +0x3584).
  static const bool crop = REXCVAR_GET(banjo_renut_scaler_crop);
  std::array<i32, 4> scaler{};
  if (crop) {
    const be_u32 *p = parameters ? GuestPtr<const be_u32>(parameters)
                                 : (device_va ? GuestPtr<const be_u32>(device_va + 0x3584) : nullptr);
    if (p)
      for (int i = 0; i < 4; ++i)
        scaler[i] = i32(u32(p[i]));
  }
  render_thread::QueueFrame([front_buffer, swap_ordinal, scaler] {
    PresentImage image;
    {
      std::lock_guard lock(State().mutex);
      image = crop ? FrontBufferImageLocked(front_buffer, scaler.data()) : FrontBufferImageLocked(front_buffer);
      if (image.texture)
        State().last_front_buffer = front_buffer;
      if (!after) {
        AdvanceTextureFrameLocked();
        AdvanceStreamCacheFrameLocked();
        LatchResolutionScaleLocked();
      }
    }
    if (!after) {
      MaybeReportMemory();
      ReleaseIdlePrebuiltPipelines();  // banjo-nx (2026-10-28, the heap)
      ReleaseSpareBuffersWhenHeapShort();
    }
    // The front buffer is the texture the game resolved the frame into.
    Video::NotePresentingSwap(swap_ordinal);  // banjo-nx (2026-10-20): the probe's "input -> GPU done"
    Video::Present(image.texture ? &image : nullptr);
    if (after) {
      rex::latency::NotePresented(swap_ordinal);  // banjo-nx (2026-10-19, input latency)
      banjo::display_sync::NotePresented(swap_ordinal);  // banjo-nx (2026-10-19, latency batch 3)
      {
        std::lock_guard lock(State().mutex);
        ::renut::gpu::SyncPipelineListLocked();  // banjo-nx (2026-10-20): the list's new records to the card
        AdvanceTextureFrameLocked();
        AdvanceStreamCacheFrameLocked();
        LatchResolutionScaleLocked();
      }
      MaybeReportMemory();
      ReleaseIdlePrebuiltPipelines();  // banjo-nx (2026-10-28, the heap)
      ReleaseSpareBuffersWhenHeapShort();
      return;
    }
    {
      std::lock_guard lock(State().mutex);
      ::renut::gpu::SyncPipelineListLocked();  // banjo-nx (2026-10-20): the list's new records to the card
    }
    rex::latency::NotePresented(swap_ordinal);  // banjo-nx (2026-10-19, input latency)
    banjo::display_sync::NotePresented(swap_ordinal);  // banjo-nx (2026-10-19, latency batch 3)
  });
  RunReadyCallbacks();

  if (device) {
    native::RewindRing(native::DeviceAt(device_va));
    native::ResetAllState(native::DeviceAt(device_va));
  }
  renutFrameLimit();  // katamari-nx: the player's frame_cap; the game's frames end here (it never calls Present)
}

// void D3DDevice_InsertCallback(D3DDevice*, D3DCALLBACKTYPE, D3DCALLBACK pCallback,
//                               DWORD Context) [0x821CAF18]
// The GPU would call pCallback(Context) on reaching this point in the ring,
// and the caller may reuse whatever the preceding work reads once it runs: it
// runs once the render thread has read that work's data (callbacks.h). N&B's
// callers: EndTiling (D3D's WorkerDpc) and the XMV renderer
// (CCalVideoRenderer::Render 0x82A6A4C0, which counts frames in flight until
// IXMediaXmvPlayer_Resume and frees itself after the last).
void D3DDevice_InsertCallback_hook(u32 device, u32 /*type*/, u32 callback, u32 context) {
  if (!callback)
    return;
  FlushDeferredVertices(device);
  RunReadyCallbacks();
  InsertCallback(callback, context);
}

// ---------------------------------------------------------------------------
// Device queries, thread ownership, presentation state
// ---------------------------------------------------------------------------

constexpr u32 kDeviceOffsetReferenceCount = 0x3C;
constexpr u32 kDeviceOffsetOwnerThread = 0x2A88;
constexpr u32 kDeviceCapsVa = 0x820FC1F8;  // N&B's constant D3DCAPS9 (katamari-nx: 0x82161B30->0x820FC1F8)
constexpr u32 kDeviceCapsSize = 0x130;
// katamari-nx: XDK 2.0.5632 has no D3D_IsFusion and no such word (N&B's 0x82DC67D4); 0 = not a Fusion GPU.
constexpr u32 kFusionVa = 0;      // non-zero on a Fusion GPU

// ULONG D3DDevice_AddRef(D3DDevice*) [0x82634AD0]
u32 D3DDevice_AddRef_hook(u32 device_va) {
  native::Device d = native::DeviceAt(device_va);
  if (!d)
    return 0;
  const u32 count = d.W(kDeviceOffsetReferenceCount) + 1;
  d.SetW(kDeviceOffsetReferenceCount, count);
  return count;
}

// void D3DDevice_AcquireThreadOwnership(D3DDevice*) [0x82634B40]
void D3DDevice_AcquireThreadOwnership_hook(u32 device_va) {
  native::Device d = native::DeviceAt(device_va);
  if (d)
    d.SetW(kDeviceOffsetOwnerThread, GuestGetCurrentThreadId());
}

// void D3DDevice_ReleaseThreadOwnership(D3DDevice*) [0x82634B80]
void D3DDevice_ReleaseThreadOwnership_hook(u32 device_va) {
  native::Device d = native::DeviceAt(device_va);
  std::atomic_thread_fence(std::memory_order_seq_cst);  // eieio
  if (d)
    d.SetW(kDeviceOffsetOwnerThread, 0);
}

// void D3DDevice_GetDeviceCaps(D3DDevice*, D3DCAPS9*) [0x82641480]
void D3DDevice_GetDeviceCaps_hook(u32 /*device*/, u32 caps_va) {
  auto *caps = GuestPtr<u8>(caps_va);
  const auto *source = GuestPtr<const u8>(kDeviceCapsVa);
  if (caps && source)
    std::memcpy(caps, source, kDeviceCapsSize);
}

// void D3DDevice_GetDisplayMode(D3DDevice*, UINT iSwapChain, D3DDISPLAYMODE*)
// [0x82641498]: width, height, refresh rate, format.
void D3DDevice_GetDisplayMode_hook(u32 device_va, u32 /*swap_chain*/, mapped_u32 mode) {
  const native::Device d = native::DeviceAt(device_va);
  if (!d || !mode)
    return;
  mode[0] = d.W(0x34D0);  // katamari-nx: 0x34E8->0x34D0
  mode[1] = d.W(0x34D4);  // katamari-nx: 0x34EC->0x34D4
  mode[2] = d.W(0x5418);  // katamari-nx: 0x5434->0x5418
  mode[3] = d.W(0x34D8);  // katamari-nx: 0x34F0->0x34D8
}

// void D3DDevice_BlockOnFence(DWORD Fence) [0x8225D1D0]: on the device D3D
// finds through VdGlobalDevice.
void D3DDevice_BlockOnFence_hook(u32 fence) {
  if (fence)
    native::BlockOnFence(native::GlobalDevice(), fence);
}

// void D3D::SynchronizeToPresentationInterval(CDevice*, BOOL) [0x82258778]:
// its swap callback (SwapCallback) and the waits around it go into the ring;
// the swap is then pending.
void D3D_SynchronizeToPresentationInterval_hook(u32 device_va, u32 /*immediate*/) {
  native::Device d = native::DeviceAt(device_va);
  if (!d)
    return;
  native::SetPredication(d, 0);
  d.SetB(native::off::Flags1, d.B(native::off::Flags1) | d3d::kSwapFlagPending);
}

// void D3DResource_GetDevice(D3DResource*, D3DDevice**) [0x8263D4B8]: the
// device D3D finds through VdGlobalDevice, referenced.
void D3DResource_GetDevice_hook(u32 /*resource*/, mapped_u32 out_device) {
  const u32 device = native::GlobalDevice();
  if (device)
    D3DDevice_AddRef_hook(device);
  if (out_device)
    *out_device = device;
}

// BOOL D3D::IsFusion() [0x82BC8078]
u32 D3D_IsFusion_hook() {
  return kFusionVa && LoadGuestU32(kFusionVa) != 0 ? 1 : 0;
}

// D3D's gamma tables (sub_82659140, _Gamma_sRGB 0x82659090): 1024 entries
// taking a 10-bit value through the display's gamma (VdGetCurrentDisplayGamma).
// Inverse goes from the display's curve to linear. D3D caches the table per
// display gamma (unk_82FB2558); it is computed per call here.
float Fmsubs(float a, float b, float c) {
  return std::fmaf(a, b, -c);
}

float Bits(u32 bits) {
  return std::bit_cast<float>(bits);
}

float GammaSRGB(float v, bool inverse) {
  if (inverse) {
    return v > 0.04045 ? float(std::pow(double((v + Bits(0x3D6147AE)) * Bits(0x3F72A76F)), 2.4000000953674316))
                       : v * Bits(0x3D9E8391);
  }
  return v > 0.0031308 ? Fmsubs(float(std::pow(double(v), 0.4166666567325592)), Bits(0x3F870A3D),
                                Bits(0x3D6147AE))
                       : v * Bits(0x414EB852);
}

std::array<u16, 1024> GammaTable(bool inverse) {
  // Guest words for the kernel's outputs.
  static const u32 scratch = [] {
    void *host = records::AllocRaw(8);
    return host ? REX_KERNEL_MEMORY()->HostToGuestVirtual(host) : 0u;
  }();
  u32 type = 0;
  float gamma = 0.0f;
  if (scratch) {
    VdGetCurrentDisplayGamma(scratch, scratch + 4);
    type = LoadGuestU32(scratch);
    gamma = std::bit_cast<float>(LoadGuestU32(scratch + 4));
  }
  std::array<u16, 1024> table{};
  for (u32 i = 0; i < 1024; ++i) {
    float v = float(i) * Bits(0x3A802008);  // flt_8219F708, 1 / 1023
    if (!inverse) {
      v = v > 0.04045 ? float(std::pow(double((v + Bits(0x3D6147AE)) * Bits(0x3F72A76F)), 2.4000000953674316))
                      : v * Bits(0x3D9E8391);
    }
    float out;
    if (type == 1) {
      out = GammaSRGB(v, inverse);
    } else if (type == 3) {
      out = float(std::pow(double(v), double(inverse ? gamma : 1.0f / gamma)));
    } else if (inverse) {
      out = v < 0.0812 ? v * Bits(0x3E638E39)
                       : float(std::pow(double((v + Bits(0x3DCAC083)) * Bits(0x3F68F065)), 2.222222328186035));
    } else {
      out = v < 0.018 ? v * 4.5f
                      : Fmsubs(float(std::pow(double(v), 0.44999998807907104)), Bits(0x3F8CAC08),
                               Bits(0x3DCAC083));
    }
    if (inverse) {
      out = out > 0.0031308 ? Fmsubs(float(std::pow(double(out), 0.4166666567325592)), Bits(0x3F870A3D),
                                     Bits(0x3D6147AE))
                            : out * Bits(0x414EB852);
    }
    const i32 value = native::TruncateToI32(std::fma(double(out), 1023.0, 0.5));
    table[i] = u16(std::clamp(value, 0, 0x3FF));
  }
  return table;
}

// The device's gamma (+0x3A9C, red, green, blue: 256 words each) for the
// display to pick up; the renderer presents without it.
constexpr u32 kDeviceOffsetGammaRamp = 0x3A84;  // katamari-nx: 0x3A9C->0x3A84

void StoreGamma(native::Device d, const be_u16 (&corrected)[768]) {
  if (std::memcmp(corrected, d.bytes + kDeviceOffsetGammaRamp, sizeof(corrected)) == 0)
    return;
  if (KeGetCurrentProcessType() != 2) {
    // The display's pending copy takes the previous ramp first.
    const u32 pending = d.W(0x56E4);  // katamari-nx: 0x5D90->0x56E4
    if (pending && !(d.W(0x56E8) & 0x80000000u)) {  // katamari-nx: 0x5D94->0x56E8
      if (auto *dest = GuestPtr<u8>(pending))
        std::memcpy(dest, d.bytes + kDeviceOffsetGammaRamp, sizeof(corrected));
    }
    d.SetW(0x56E8, d.W(0x56E8) | 0x80000000u);  // katamari-nx: 0x5D94->0x56E8, 0x5D94->0x56E8
  }
  std::memcpy(d.bytes + kDeviceOffsetGammaRamp, corrected, sizeof(corrected));
  d.SetW(0x412C, d.W(0x412C) | 2);  // katamari-nx: 0x4144->0x412C, 0x4144->0x412C
}

// D3DDevice_SetGammaRamp's work: the ramp (D3DGAMMARAMP, 3 x 256 words)
// through the gamma table (GammaRampCorrected 0x826593F8).
void SetGammaRamp(native::Device d, const u16 *ramp) {
  g_gamma_piecewise = false;  // halo3-nx
  const std::array<u16, 1024> table = GammaTable(false);
  be_u16 corrected[768];
  for (u32 i = 0; i < 768; ++i)
    corrected[i] = u16((table[ramp[i] >> 6] & 0x3FF) << 6);
  StoreGamma(d, corrected);
}

// D3DDevice_SetPWLGamma's: D3DPWLGAMMA (3 x 128 {base, delta}) through the
// table (sub_82659490), each segment's end clamped to 0x3FF.
void SetPWLGamma(native::Device d, const u16 *pwl) {
  g_gamma_piecewise = true;  // halo3-nx
  const std::array<u16, 1024> table = GammaTable(false);
  be_u16 corrected[768];
  for (u32 c = 0; c < 3; ++c) {
    for (u32 i = 0; i < 128; ++i) {
      const u32 at = c * 256 + i * 2;
      const u32 base = pwl[at] >> 6;
      const u32 end = std::min(base + (pwl[at + 1] >> 6), 0x3FFu);
      corrected[at] = u16((table[base] & 0x3FF) << 6);
      corrected[at + 1] = u16(((table[end] - table[base]) & 0x3FF) << 6);
    }
  }
  StoreGamma(d, corrected);
}

void ReadRamp(u32 ramp_va, u16 (&ramp)[768]) {
  const auto *guest = GuestPtr<const be_u16>(ramp_va);
  for (u32 i = 0; i < 768; ++i)
    ramp[i] = guest ? u16(guest[i]) : 0;
}

// void D3DDevice_SetGammaRamp(D3DDevice*, const D3DGAMMARAMP*) [0x8261FA08]
// (TU3's build dropped the unused Flags between them)
void D3DDevice_SetGammaRamp_hook(u32 device_va, u32 ramp_va) {
  native::Device d = native::DeviceAt(device_va);
  if (!d || !ramp_va)
    return;
  u16 ramp[768];
  ReadRamp(ramp_va, ramp);
  SetGammaRamp(d, ramp);
}

// Port 173 (from rehatched-nx, its commit 9d93d896): XDK 2.0.9407 (and 11164, docs/xdk-notes.md) keeps the Flags
// argument, (D3DDevice*, DWORD Flags, ramp): the two-argument hooks would read Flags as the ramp's address. Not bound
// here (Katamari's build dropped Flags); a port at such an XDK binds these at the two functions' addresses in place of
// the two-argument hooks (BANJO_RENUT_HOOK(sub_<address>, D3DDevice_SetGammaRamp_flags_hook), and the PWL one).
void D3DDevice_SetGammaRamp_hook(u32 device_va, u32 ramp_va);
void D3DDevice_SetPWLGamma_hook(u32 device_va, u32 pwl_va);
[[maybe_unused]] void D3DDevice_SetGammaRamp_flags_hook(u32 device_va, u32 /*flags*/, u32 ramp_va) {
  D3DDevice_SetGammaRamp_hook(device_va, ramp_va);
}
[[maybe_unused]] void D3DDevice_SetPWLGamma_flags_hook(u32 device_va, u32 /*flags*/, u32 pwl_va) {
  D3DDevice_SetPWLGamma_hook(device_va, pwl_va);
}

// void D3DDevice_SetPWLGamma(D3DDevice*, const D3DPWLGAMMA*) [0x8261FAD0]
// (TU3's build dropped the unused Flags between them)
void D3DDevice_SetPWLGamma_hook(u32 device_va, u32 pwl_va) {
  native::Device d = native::DeviceAt(device_va);
  if (!d || !pwl_va)
    return;
  u16 pwl[768];
  ReadRamp(pwl_va, pwl);
  SetPWLGamma(d, pwl);
}

// The D3D internals Direct3D_CreateDevice and D3DDevice_Release run, for any
// other caller.
void D3D_XenonGpu_Initialize_hook(u32 device) {
  XenonGpuInitialize(native::DeviceAt(device));
}
void D3D_InitializeApiState_hook(u32 device) {
  InitializeApiState(native::DeviceAt(device));
}
u32 D3D_InitializePresentationParameters_hook(u32 device, u32 parameters) {
  return InitializePresentationParameters(native::DeviceAt(device), parameters) ? 1 : 0;
}
void D3D_CDevice_Uninitialize_hook(u32 device) {
  UninitializeDevice(native::DeviceAt(device));
}

} // namespace

namespace renut::gpu::native {

void RewindRing(Device device) {
  if (!device || !g_ring_sink)
    return;
  device.SetW(off::Ring, g_ring_sink);
  device.SetW(off::RingLimit, g_ring_sink + kRingSinkSize);
  device.SetW(off::RingGuarantee, g_ring_sink + kRingSinkSize - kRingSinkGuaranteeMargin);
}

u32 KickOff(u32 device_va) {
  return KickOff_hook(device_va);
}

void BlockOnFence(u32 device_va, u32 fence) {
  BlockOnFence_hook(device_va, fence, 0, 0);
}

u32 GlobalDevice() {
  const u32 global = LoadGuestU32(kVdGlobalDeviceImportSlot);
  return global ? LoadGuestU32(global) : 0;
}

void Swap(u32 device_va, u32 front_buffer, u32 parameters) {
  D3DDevice_Swap_hook(device_va, front_buffer, parameters);
}

// CDevice::SetFence (0x82246420): with immediate fences the device writes the
// completion itself (unless +0x541C is set); otherwise the fence completes on
// the render thread, as a KickOff's. A device recording a command buffer
// would put it into the buffer, which runs on the host.
void SetFence(u32 device_va) {
  auto *bytes = GuestPtr<u8>(device_va);
  if (!bytes)
    return;
  if (bytes[kDeviceOffsetFlags2] & kDeviceFlagImmediateFences) {
    Device d = DeviceAt(device_va);
    const u32 fence = d.W(off::FenceTag);
    if (!d.W(0x5404)) {  // katamari-nx: 0x541C->0x5404
      if (auto *completion = GuestPtr<be_u32>(d.W(off::FenceCompletion))) {
        completion[0] = fence;
        completion[1] = (d.W(0x3A30) & 3) | d.W(off::Ring);  // katamari-nx: 0x3A48->0x3A30
      }
    }
    d.SetW(off::FenceTag, fence + 2);
    return;
  }
  if (!IsRecordingCommandBuffer(device_va))
    KickDevice(GuestPtr<d3d::Device>(device_va), device_va);
}

} // namespace renut::gpu::native

BANJO_RENUT_HOOK(sub_824E0590, D3DDevice_InsertCallback_hook);  // D3DDevice_InsertCallback  // katamari-nx: N&B sub_821CAF18, signature 0.88 (next 0.59)
BANJO_RENUT_HOOK(sub_824D01C8, D3DDevice_AddRef_hook);  // D3DDevice_AddRef  // katamari-nx: N&B sub_82634AD0, signature 1.00 (next 0.43)
BANJO_RENUT_HOOK(sub_824D0430, D3DDevice_AcquireThreadOwnership_hook);  // D3DDevice_AcquireThreadOwnership  // katamari-nx: N&B sub_82634B40, signature 1.00 (next 0.78)
BANJO_RENUT_HOOK(sub_824D0470, D3DDevice_ReleaseThreadOwnership_hook);  // D3DDevice_ReleaseThreadOwnership  // katamari-nx: N&B sub_82634B80, signature 1.00 (next 0.46)
BANJO_RENUT_HOOK(sub_824E6410, D3DDevice_GetDeviceCaps_hook);  // D3DDevice_GetDeviceCaps  // katamari-nx: N&B sub_82641480, D3DDevice_GetDeviceCaps: memcpy(out, 0x820FC1F8, 304), N&B's memcpy(out, 0x82161B30, 304)
BANJO_RENUT_UNHOOKED_HOOK(sub_82641498, D3DDevice_GetDisplayMode_hook);  // D3DDevice_GetDisplayMode  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824E67A0 0.38, next 0.35))
BANJO_RENUT_UNHOOKED_HOOK(sub_8225D1D0, D3DDevice_BlockOnFence_hook);  // D3DDevice_BlockOnFence  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824E8610 0.51, next 0.51))
BANJO_RENUT_HOOK(sub_824E89E8, D3D_SynchronizeToPresentationInterval_hook);  // D3D_SynchronizeToPresentationInterval  // katamari-nx: N&B sub_82258778, D3D_SynchronizeToPresentationInterval: called by Present before the resolve; sig 0.72/0.39
BANJO_RENUT_UNHOOKED_HOOK(sub_8263D4B8, D3DResource_GetDevice_hook);  // D3DResource_GetDevice  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824E8610 0.43, next 0.43))
BANJO_RENUT_UNHOOKED_HOOK(sub_82BC8078, D3D_IsFusion_hook);  // D3D_IsFusion  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824903A8 0.56, next 0.43))
BANJO_RENUT_HOOK(sub_824DE218, D3DDevice_SetGammaRamp_hook);  // D3DDevice_SetGammaRamp  // katamari-nx: N&B sub_82636F48, D3DDevice_SetGammaRamp: InitializePresentationParameters calls it, then SetPWLGamma, in N&B's order; sig 0.63 for both
BANJO_RENUT_HOOK(sub_824DE2F8, D3DDevice_SetPWLGamma_hook);  // D3DDevice_SetPWLGamma  // katamari-nx: N&B sub_82637018, D3DDevice_SetPWLGamma: as above
BANJO_RENUT_HOOK(sub_824D0598, Direct3D_CreateDevice_hook);  // Direct3D_CreateDevice  // katamari-nx: N&B sub_82634990, Direct3D_CreateDevice: allocates the 0x5700 device (li r3,22272 / 128 alignment, sub_824D0240), calls the device init sub_824E18D0 (XenonGpu_Initialize, InitializePresentationParameters, InitializeApiState); sig 0.39 because N&B inlines that init
BANJO_RENUT_HOOK(sub_824D0368, D3DDevice_Release_hook);  // D3DDevice_Release  // katamari-nx: N&B sub_82634AE8, signature 1.00 (next 0.59)
BANJO_RENUT_HOOK(sub_824ED278, D3D_XenonGpu_Initialize_hook);  // D3D_XenonGpu_Initialize  // katamari-nx: N&B sub_8264B748, D3D_XenonGpu_Initialize: called by the device init sub_824E18D0 after the -1 words; sig 0.66/0.23
BANJO_RENUT_HOOK(sub_824E11A0, D3D_InitializeApiState_hook);  // D3D_InitializeApiState  // katamari-nx: N&B sub_82641BC8, D3D_InitializeApiState: called by the device init; reads PresentInterval +13572 and BehaviorFlags +22264; sig 0.58/0.25
BANJO_RENUT_HOOK(sub_824E1408, D3D_InitializePresentationParameters_hook);  // D3D_InitializePresentationParameters  // katamari-nx: N&B sub_82641EA8, D3D_InitializePresentationParameters: makes the auto surfaces (+14800..14808) with CreateTexture / CreateSurface; sig 0.65/0.27
BANJO_RENUT_HOOK(sub_824E1B50, D3D_CDevice_Uninitialize_hook);  // D3D_CDevice_Uninitialize  // katamari-nx: N&B sub_82642568, D3D_CDevice_Uninitialize: VdShutdownEngines, VdSetGraphicsInterruptCallback; sig 0.40/0.26
BANJO_RENUT_HOOK(sub_824E0298, KickOff_hook);  // D3D_CDevice_KickOff  // katamari-nx: N&B sub_821DFFF0, signature 0.80 (next 0.41)
BANJO_RENUT_HOOK(sub_824E0110, KickOffSegment_hook);  // D3D_CDevice_KickOffSegment  // katamari-nx: N&B sub_821DFC08, D3D_CDevice_KickOffSegment: sig 0.71/0.31; CommandBuffer +13492, InKickOff +13216 as the zones say
BANJO_RENUT_HOOK(sub_824E04B0, BeginRingBig_hook);  // D3D_CDevice_BeginRingBig  // katamari-nx: N&B sub_821C7360, D3D_CDevice_BeginRingBig: sig 0.67/0.32
BANJO_RENUT_HOOK(sub_8255A200, BeginRingManual_hook);  // D3D_CDevice_BeginRingManual  // katamari-nx: N&B sub_82A6DD88, signature 1.00 (next 0.46)
BANJO_RENUT_HOOK(sub_824DF3F0, RingBufferDeviceAllocate_hook);  // D3D_RingBufferDeviceAllocate  // katamari-nx: N&B sub_821FC4B8, D3D_RingBufferDeviceAllocate: sig 0.61/0.31
BANJO_RENUT_UNHOOKED_HOOK(sub_821FC318, AllocateInlineData_hook);  // D3D_CDevice_AllocateInlineData  // katamari-nx: not hooked (signature too weak or ambiguous (sub_824DFF98 0.38, next 0.31))
BANJO_RENUT_HOOK(sub_824E05E8, D3DDevice_SetRingBufferParameters_hook);  // D3DDevice_SetRingBufferParameters  // katamari-nx: N&B sub_82640090, D3DDevice_SetRingBufferParameters: the VdInitializeRingBuffer caller; sig 0.70/0.21
BANJO_RENUT_HOOK(sub_824E8E90, D3DDevice_Swap_hook);  // D3DDevice_Swap  // katamari-nx: N&B sub_822554F8, D3DDevice_Swap: the only VdSwap caller; sig 0.53/0.26
BANJO_RENUT_UNHOOKED_HOOK_SHARED(sub_82640818, D3DDevice_BeginCommandBuffer_hook);  // D3DDevice_BeginCommandBuffer  // katamari-nx: not hooked (D3DDevice_BeginCommandBuffer: no clear counterpart (sig 0.27); census first)
BANJO_RENUT_UNHOOKED_HOOK(sub_82640DE8, D3DDevice_EndCommandBuffer_hook);  // D3DDevice_EndCommandBuffer  // katamari-nx: not hooked (D3DDevice_EndCommandBuffer: no clear counterpart (sig 0.49/0.48))
BANJO_RENUT_UNHOOKED_HOOK_SHARED(sub_821F1A78, D3DDevice_RunCommandBuffer_hook);  // D3DDevice_RunCommandBuffer  // katamari-nx: not hooked (D3DDevice_RunCommandBuffer: no clear counterpart (sig 0.50/0.48))
BANJO_RENUT_HOOK(sub_824DFC48, BlockOnFence_hook);  // D3D_CDevice_BlockOnFence  // katamari-nx: N&B sub_8225D370, D3D_CDevice_BlockOnFence: sig 0.55/0.32
BANJO_RENUT_HOOK(sub_824D0290, CBlocker_Check_hook);  // D3D_CBlocker_Check  // katamari-nx: N&B sub_82634740, D3D_CBlocker_Check: sig 0.66/0.26
BANJO_RENUT_HOOK(sub_824E03D0, IsFencePending_hook);  // D3DDevice_IsFencePending  // katamari-nx: N&B sub_8225D468, D3DDevice_IsFencePending: sig 0.71/0.47
