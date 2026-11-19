/**
 * @file    gpu/render/surfaces.cpp
 */
#include "renut_engine/gpu/render/surfaces.h"

#include <algorithm>
#include <atomic>
#include <chrono>  // banjo-nx (2026-10-28): dynamic resolution
#include <numeric>
#include <string>
#include <mutex>  // blur-nx (2026-11-10, port 76; its key 215): g_stack_headers_mutex
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"
#include "renut_engine/gpu/ultimate.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/render/format.h"
#include "renut_engine/gpu/render/graphics_options.h"
#include "renut_engine/gpu/render/textures.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/guest/widescreen.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/perf/frame_stats.h"  // sasrt-nx (2026-11-09, sasrt_dynamic_cpu_aware; port 52)
#include "renut_engine/gpu/sasrt_gpu_round.h"  // sasrt-nx (2026-11-09, sasrt_rt_restore_candidates; port 54)
#include <rex/filesystem/devices/host_path_file.h>  // sasrt-nx (2026-11-09; port 58): TrimRangeCache (the governor)
#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>  // banjo-nx (2026-10-21, from sa2-nx 1.0.3.1): Deko3DSetSwapChainSize
#endif
#if defined(__SWITCH__)
#include <switch.h>
#include <rex/ui/switch_saltynx.h>  // banjo-nx (2026-10-21): the console's mode, Reverse-NX obeyed
#include <threading_switch.h>  // sasrt-nx (2026-11-09; port 58): RexSwitchReleaseSpareStacks (the governor)
#endif

// renut-ultimate (2026-11-16, port 145; tw2de_nx's of 2026-10-22, ON at the user's word: "port the speed ups as on by default").
// Port 174 (from rehatched-nx): see CapturedHeaderVa. Accuracy, so off by default; a game that re-points headers turns it
// on in its toml, with banjo_renut_chain_reuse and banjo_deko3d_scanout_handover off (both read the live header:
// rehatched-nx turned them off for Angry Birds).
REXCVAR_DEFINE_BOOL(banjo_renut_all_surface_headers, false, "Banjo/Renut",
                    "From rehatched-nx: the header of every bound surface (not only those on the stack, "
                    "blur_renut_stack_surface_headers) is kept as the game bound it, with each draw, clear and resolve, "
                    "and the render thread reads that (a game that re-points a heap header between EDRAM views drew to "
                    "the wrong view). Pair it with banjo_renut_chain_reuse and banjo_deko3d_scanout_handover off. "
                    "false: a heap header is read as it is when the render thread gets to it, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_keep_unused_targets, true, "Banjo/Renut",
                    "tw2de_nx: a render target no surface uses any more is kept for 250 ms for a surface that takes the "
                    "same EDRAM view again, instead of being destroyed and made anew (a game that switches one surface "
                    "between two views every frame: tw2de_nx made 8 images a frame, 4 % of a core in the kernel's "
                    "mapping calls). false: retired at once, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_renut_kept_targets, 24, "Banjo/Renut",
                     "With banjo_renut_keep_unused_targets: how many unused targets are kept at most (the oldest goes)");
REXCVAR_DEFINE_STRING(renut_resolution, "720p", "Nuts&Bolts/Graphics",
                      "Internal render resolution, by its height: 480p, 720p (the game's own), "
                      "1080p, 1440p or 4K (2160 lines). Applies from the next frame.")
    .allowed({"480p", "720p", "1080p", "1440p", "4K"});

namespace renut::gpu {

void ForgetBoundTargetsLocked();  // sasrt-nx (2026-10-20): render/draw.cpp

namespace {

// ResolutionScale, once latched.
RenderScale g_resolution_scale;
bool g_resolution_latched = false;
// sasrt-nx (2026-11-09 night, sasrt_retire_stale_sets; port 58): the 3D's scale before its last change, and the MB of
// render targets at the 3D's own scale (RetireIdleTargetsLocked's walk, every 10 frames; sasrt_dynamic_up_memory).
RenderScale g_prev_resolution_scale;
std::atomic<uint32_t> g_current_set_mb{0};

// banjo-nx (2026-10-21, internal resolution): UiScale, the composite's UI target and this frame's redirect to it, and how
// many frames in a row have had no composite (kMenuFrames of them: menus or loading, drawn at the display's resolution
// whole; from the start too, so the boot logos and title are).
constexpr uint32_t kMenuFrames = 30;
RenderScale g_ui_scale;
// The internal heights (render_scale.h codes 4 and 6), as the shaders read them: handheld | docked << 16.
uint32_t g_internal_heights = 720u | 720u << 16;
// banjo-nx (2026-10-28, GPU block, banjo_renut_dynamic_resolution): each mode's heights, lowest first (night_gpu5: the
// range's ends and banjo_renut_dynamic_steps between them), how many (0: no range for that mode; set once by
// LatchResolutionScaleLocked), the one in use (an index into the mode's) and the mode it is for.
constexpr int kMaxDynamicSteps = 10;
int32_t g_dynamic_steps[2][kMaxDynamicSteps] = {};
int g_dynamic_count[2] = {0, 0};
std::atomic<int> g_dynamic_step{0};
std::atomic<bool> g_dynamic_docked{false};
// banjo-nx (2026-10-28, night_gpu5): the heights render_scale.h's settable codes 4 and 6 hold now ([0] code 4, [1] code
// 6; the shaders read them from InternalHeightsWord), and the frame each was last the render scale. A height other than
// 720 (code 0) and 1080 (code 2) takes the code that holds it, or else the other code than the one in use, whose targets
// go first (CodeForHeightLocked): a range's steps take turns on the two codes.
int32_t g_code_height[2] = {0, 0};
uint64_t g_code_used[2] = {0, 0};
uint64_t g_latches = 0;
struct UiTarget {
  HostSurface *scene = nullptr;  // the composite's target it was made for, and that target's generation then
  uint64_t scene_generation = 0;
  std::unique_ptr<HostSurface> surface;
  bool redirect = false;
};
// The composite's target's, and (made when a draw after the composite tests depth: the menus' 3D icons) the depth
// target's bound with it.
UiTarget g_ui, g_ui_depth;
bool g_composite_seen = false;
uint32_t g_frames_without_composite = kMenuFrames;

struct FramebufferKey {
  const HostSurface *colors[d3d::kMaxRenderTargets] = {};
  uint64_t color_generations[d3d::kMaxRenderTargets] = {};
  const HostSurface *depth = nullptr;
  uint64_t depth_generation = 0;

  bool operator==(const FramebufferKey &o) const {
    for (uint32_t i = 0; i < d3d::kMaxRenderTargets; ++i) {
      if (colors[i] != o.colors[i] || color_generations[i] != o.color_generations[i])
        return false;
    }
    return depth == o.depth && depth_generation == o.depth_generation;
  }
};

struct CachedFramebuffer {
  FramebufferKey key;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
};

// A D3DSurface is a view of EDRAM: its base tile (RB_COLOR_INFO /
// RB_DEPTH_INFO bits 0-11), pitch (RB_SURFACE_INFO bits 0-13) and sample
// count. Surface objects over the same EDRAM with the same layout see the same
// pixels on the GPU, and N&B relies on it: the depth surface its after-effects
// pass binds is a second object over the scene's depth buffer (the character
// holograms test against it), and each hologram renders through surfaces of
// its own at the same EDRAM bases. So host targets belong to the EDRAM view,
// shared by every surface object over it, as long as they also agree on the
// host format and render scale: graphics_options renders some surfaces at
// another scale (the post-processing buffers at 1x), and a view used by
// surfaces of both kinds would otherwise be rebuilt at every switch.
// blur-nx (2026-11-18, the game menu: MSAA at once): msaa_level as taken at this frame's latch (0 before the first);
// part of an MSAA target's key, so a change makes those targets anew at their next use.
uint32_t g_msaa_latched = 0;

uint64_t TargetKey(const d3d::SurfaceDesc &desc, bool depth, plume::RenderFormat format,
                   RenderScale scale) {
  const u32 info = desc.SurfaceInfo;
  const uint64_t msaa = ((info >> 16) & 3) == 0 ? 0 : g_msaa_latched >= 4 ? 2 : g_msaa_latched >= 2 ? 1 : 0;
  return uint64_t(u32(desc.ColorInfo) & 0xFFF) | uint64_t(info & 0x3FFF) << 12 |
         uint64_t((info >> 16) & 3) << 26 | uint64_t(depth) << 28 |
         uint64_t(scale.code & 7) << 29 | uint64_t(u32(format) & 0xFFFFFF) << 32 | msaa << 56;
}

std::unordered_map<uint64_t, std::unique_ptr<HostSurface>> g_targets;
// Surface VA -> its target's key, and how many surfaces use each target.
std::unordered_map<u32, uint64_t> g_surface_keys;
std::unordered_map<uint64_t, uint32_t> g_target_users;
// renut-ultimate (2026-11-16, port 145; tw2de_nx's of 2026-10-22, banjo_renut_keep_unused_targets, on by default):
// targets no surface uses any more, kept (by key, with when the last user left) for a surface that takes the same
// view again. tw2de_nx's game rewrote one surface object between two views every frame: each switch retired one
// target and made the other anew, about 8 image creations a frame (250 GPU mappings a second, 4 % of a core in the
// kernel's mapping calls). The console's EDRAM keeps what each view held, so a kept target is also closer to it.
std::unordered_map<uint64_t, int64_t> g_unused_targets;
std::vector<CachedFramebuffer> g_framebuffers;
// banjo-nx (2026-10-19, render-thread proposal 8): the last framebuffers looked up, checked before the search of every
// framebuffer made (a linear search with an 80-byte compare at every draw's BindTargets); emptied whenever a cached
// framebuffer is retired (g_framebuffer_epoch).
struct RecentFramebuffer {
  FramebufferKey key;
  plume::RenderFramebuffer *framebuffer = nullptr;
  uint64_t epoch = 0;
};
RecentFramebuffer g_recent_framebuffers[4];
uint64_t g_framebuffer_epoch = 1;
uint64_t g_next_generation = 1;
// banjo-nx (2026-10-19, banjo_deko3d_depth24): the D3D depth formats each EDRAM depth view has been bound as (1 D24S8,
// 2 D24FS8). The host format is part of a target's key, so a view used as both would get two targets where the console
// has one memory: once a view has been bound as D24FS8, every surface over it takes the 32-bit float format.
std::unordered_map<uint64_t, uint8_t> g_depth_view_formats;
// SetTilingExtentLocked; 0 x 0 when not tiling.
uint32_t g_tiling_width = 0;
uint32_t g_tiling_height = 0;

// Drops cached framebuffers that reference 'surface'.
void ForgetFramebuffers(const HostSurface *surface) {
  for (size_t i = 0; i < g_framebuffers.size();) {
    const FramebufferKey &key = g_framebuffers[i].key;
    bool uses = key.depth == surface;
    for (const HostSurface *color : key.colors)
      uses = uses || color == surface;
    if (uses) {
      Video::RetireLocked(std::move(g_framebuffers[i].framebuffer));
      g_framebuffers[i] = std::move(g_framebuffers.back());
      g_framebuffers.pop_back();
      ++g_framebuffer_epoch;
    } else {
      ++i;
    }
  }
}

// Retires the target and everything made over it (deferred past in-flight
// frames).
// banjo-nx (2026-10-19, CPU proposal 8, banjo_renut_cpu_memos): GetHostSurfaceLocked's result per surface VA and the
// three descriptor words it reads, valid while nothing it reads has changed (g_surface_epoch: bumped by every target
// built, retired, unused or rebound, the resolution scale latched and the tiling extent set).
struct SurfaceMemo {
  // Port 161 (from fnxf_xiii, its note of 2026-11-11 from FNXF XIII-LR): color_info (the EDRAM base) too. A surface
  // object pointed, the same size, pitch and format, at another EDRAM base gave the memo's target of the old base
  // (LR at 1600x900: the scene's tile colour, then its depth's EDRAM for the per-tile depth restore), where the slow
  // path's TargetKey keys on ColorInfo.
  u32 va = 0, size = 0, info = 0, format = 0, color_info = 0;
  uint64_t epoch = 0;
  HostSurface *surface = nullptr;
};
SurfaceMemo g_surface_memo[64];  // banjo-nx (2026-10-19, render-thread proposal 8): 64, indexed by a hash of the VA
uint64_t g_surface_epoch = 1;

void FreeDepthViews(std::vector<DepthImage::View> &views) {
  for (DepthImage::View &v : views) {
    if (v.slot != UINT32_MAX && v.slot != kInvalidDescriptor)
      Video::FreeTextureSlotLocked(v.slot);
    Video::RetireLocked(std::move(v.view));
  }
  views.clear();
}

// sasrt-nx (2026-11-09, sasrt_rt_restore_candidates; port 54): moves when a target is built, retired or leaves the table
// (DepthRestoreSourceLocked's candidate lists). An epoch of its own: the surface epoch moves with every re-pointed surface
// and override change.
uint64_t g_sasrt_table_epoch = 1;

void RetireTarget(HostSurface &surface) {
  ++g_sasrt_table_epoch;  // sasrt-nx (2026-11-09, sasrt_rt_restore_candidates; port 54)
  // banjo-nx (2026-10-21, internal resolution): the composite's target goes, and its UI target with it.
  for (UiTarget *u : {&g_ui, &g_ui_depth}) {
    if (&surface == u->scene) {
      u->scene = nullptr;
      u->redirect = false;
      if (u->surface && u->surface->texture)
        RetireTarget(*u->surface);
    }
  }
  if (surface.handed)  // kameo-nx (2026-11-03): colour targets too (banjo_deko3d_color_handover)
    DepthHandOverBeforeRetireLocked(&surface);  // banjo-nx (2026-10-19, banjo_deko3d_depth_handover)
  FreeDepthViews(surface.handover_views);
  surface.stencil_used = false;
  ++g_surface_epoch;
  ForgetFramebuffers(&surface);
  if (surface.sample_slot != UINT32_MAX)
    Video::FreeTextureSlotLocked(surface.sample_slot);
  surface.sample_slot = UINT32_MAX;
  Video::RetireLocked(std::move(surface.sample_view));
  if (surface.stencil_slot != UINT32_MAX && surface.stencil_slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(surface.stencil_slot);
  surface.stencil_slot = UINT32_MAX;
  Video::RetireLocked(std::move(surface.stencil_view));
  Video::RetireLocked(std::move(surface.texture));
}

plume::RenderSampleCounts SampleCount(u32 info_word) {
  switch ((info_word >> 16) & 3) {
  case 1:
    return plume::RenderSampleCount::COUNT_2;
  case 2:
    return plume::RenderSampleCount::COUNT_4;
  default:
    return plume::RenderSampleCount::COUNT_1;
  }
}

// At least 'min_width' x 'min_height' (see SetTilingExtentLocked), at 'scale'.
bool BuildTarget(HostSurface &surface, const d3d::SurfaceDesc &desc, uint32_t min_width,
                 uint32_t min_height, RenderScale scale) {
  surface.size_word = desc.Size;
  surface.info_word = desc.SurfaceInfo;
  surface.format_word = desc.Format;
  surface.color_info_word = desc.ColorInfo;  // halo3-nx
  surface.width = std::max(d3d::SurfaceWidth(surface.size_word), min_width);
  surface.height = std::max(d3d::SurfaceHeight(surface.size_word), min_height);
  surface.scale = scale;
  std::fill(std::begin(surface.profile_labels), std::end(surface.profile_labels), UINT32_MAX);
  const uint32_t host_width = surface.scale.Size(surface.width);
  const uint32_t host_height = surface.scale.Size(surface.height);
  surface.depth = d3d::IsDepthFormat(surface.format_word);
  surface.sasrt_fixed16 = !surface.depth && (((u32(desc.ColorInfo) >> 16) & 0xF) == 4u ||
                                             ((u32(desc.ColorInfo) >> 16) & 0xF) == 5u);  // sasrt-nx (2026-10-20)
  surface.samples = SampleCount(surface.info_word);
  // banjo-nx (2026-10-21, banjo_renut_portrait_msaa_samples, NOT exact, the user's call): the small targets the game makes
  // with MSAA (the dialogue portrait's 96 x 96 box, 4x; colour and depth alike, so a framebuffer's attachments still
  // match) with fewer samples: 2 for 2x, 1 single-sampled. The pipelines take the target's sample count; single-sampled,
  // its resolves become copies.
  if (const uint32_t want = banjo::renut_gpu::PortraitMsaaSamples();
      want && uint32_t(surface.samples) > want && surface.width <= 128 && surface.height <= 128) {
    static uint32_t logged = 0;
    if (logged++ < 8)
      RGPU_INFO("Surface {:08X}: {}x{} {} with x{} MSAA made x{} (banjo_renut_portrait_msaa_samples)", surface.guest_va,
                surface.width, surface.height, surface.depth ? "depth" : "colour", u32(surface.samples), want);
    surface.samples = want == 2 ? plume::RenderSampleCount::COUNT_2 : plume::RenderSampleCount::COUNT_1;
  }
  // katamari-nx (2026-11-02, msaa_level, the player's setting): the same for every target the game makes with MSAA.
  // 4 is the game's own (nothing changes); it only lowers a count.
  if (const uint32_t want = g_msaa_latched ? g_msaa_latched : banjo::renut_gpu::MsaaLevel();
      uint32_t(surface.samples) > want)
    surface.samples = want == 2 ? plume::RenderSampleCount::COUNT_2 : plume::RenderSampleCount::COUNT_1;
  if (surface.depth) {
    surface.format = DepthTargetFormat(surface.format_word);  // banjo-nx (2026-10-19, banjo_deko3d_depth24)
    if (banjo::renut_gpu::Depth24On()) {
      // As GetHostSurfaceSlow keys it: a view ever bound as D24FS8 is 32-bit float for every surface over it.
      const auto it = g_depth_view_formats.find(TargetKey(desc, true, plume::RenderFormat::UNKNOWN, scale));
      if (it != g_depth_view_formats.end() && (it->second & 2))
        surface.format = DepthTargetFormat();
    }
    // sasrt-nx (2026-11-11, port 75; its formats batch, row 26: renut_depth_float_no_stencil, a speed trade, off): any
    // float depth target (D24FS8) without a stencil plane, half the bytes. Stencil on it then does nothing (its uses
    // are logged: renut_diag_formats, and draw.cpp's SasrtWarnStencilOnD32f).
    if (banjo::renut_gpu::DepthFloatNoStencilOn() && surface.format == plume::RenderFormat::D32_FLOAT_S8_UINT &&
        (surface.format_word & 0x3F) == u32(GpuTextureFormat::Fmt24_8_Float)) {
      surface.format = plume::RenderFormat::D32_FLOAT;
      static uint32_t logged = 0;
      if (logged++ < 8)
        RGPU_INFO("[formats] depth target {:08X} {}x{} made D32F without stencil (renut_depth_float_no_stencil)",
                  surface.guest_va, surface.width, surface.height);
    }
    // sasrt-nx (2026-11-11, port 75; its formats batch, row 26): only that format's line (D32F is not the depth
    // format either).
    if (surface.format == plume::RenderFormat::D24_UNORM_S8_UINT) {
      static uint32_t logged = 0;
      if (logged++ < 16)
        RGPU_INFO("Depth target {:08X}: {}x{} x{} samples, 24-bit unorm depth with stencil (banjo_deko3d_depth24)",
                  surface.guest_va, surface.width, surface.height, u32(surface.samples));
    }
  } else {
    surface.format = SurfaceColorFormat(desc);
  }
  // sasrt-nx (2026-11-11, port 75; its formats batch, the diagnostic: renut_diag_formats): the first target of each
  // guest format / EDRAM colour format / host format (its size logged, not in the key: its audit's finding 8).
  if (banjo::renut_gpu::DiagFormatsOn()) {
    static std::vector<uint64_t> seen;
    const uint32_t colour_format = (u32(desc.ColorInfo) >> 16) & 0xF;
    const uint64_t key = uint64_t(surface.depth) << 63 | uint64_t(surface.format_word & 0x3F) << 50 |
                         uint64_t(colour_format) << 46 | uint64_t(surface.format) << 32;
    if (seen.size() < 96 && std::find(seen.begin(), seen.end(), key) == seen.end()) {
      seen.push_back(key);
      RGPU_INFO("[formats] {} target {}x{}: guest format {} ({}), EDRAM colour format {}, host format {}",
                surface.depth ? "depth" : "colour", surface.width, surface.height, surface.format_word & 0x3F,
                GpuTextureFormatName(surface.format_word), surface.depth ? -1 : int(colour_format),
                int(surface.format));
    }
  }

  plume::RenderDevice *device = Video::Device();
  const plume::RenderMultisampling multisampling(surface.samples);
  // Sampled too: resolves read them, and so does scan-out of a bound target.
  if (surface.depth) {
    surface.texture = device->createTexture(plume::RenderTextureDesc::DepthTarget(
        host_width, host_height, surface.format, multisampling));
  } else {
    surface.texture = device->createTexture(plume::RenderTextureDesc::ColorTarget(
        host_width, host_height, surface.format, multisampling));
  }
  surface.layout = plume::RenderTextureLayout::UNKNOWN;
  surface.generation = g_next_generation++;
  if (!TextureCreated(surface.texture.get())) {
    surface.texture.reset();
    if (!ReportDeviceLost(device)) {
      RGPU_ERROR("Surface {:08X}: {}x{} {} target creation failed", surface.guest_va,
                 host_width, host_height, surface.depth ? "depth" : "color");
    }
    return false;
  }
  surface.texture->setName(surface.depth ? "renut depth surface" : "renut color surface");
  RGPU_DEBUG("Surface {:08X}: {}x{} (x{:.3g} scale) {} format {} x{} samples", surface.guest_va,
             surface.width, surface.height, surface.scale.Factor(), surface.depth ? "depth" : "color",
             u32(surface.format), u32(surface.samples));
  // sasrt-nx (2026-10-20, sasrt_renut_deko3d_clear_new): a new colour target starts at 0 on deko3d too.
  if (IsDeko3D() && !surface.depth && banjo::renut_gpu::Deko3DClearNewOn()) {
    if (plume::RenderCommandList *cmd = Video::CommandListLocked()) {
      HostSurface *self = &surface;
      if (plume::RenderFramebuffer *fb = GetFramebufferLocked(&self, 1, nullptr)) {
        TransitionSurfaceLocked(self, plume::RenderTextureLayout::COLOR_WRITE);
        cmd->setFramebuffer(fb);
        cmd->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
        ForgetBoundTargetsLocked();  // the draw binds its own targets again
      }
    }
  }
  return true;
}

#if defined(PLUME_DEKO3D)
// banjo-nx (2026-10-28, night_gpu5, dynamic resolution steps): the scale of 'height' lines. 720 is code 0 and 1080 code
// 2 (fixed factors); any other height takes the settable code (4 or 6) that holds it, or else the one that is not the
// render scale now (the less recently used when neither is), set to the height here. That code's targets are retired
// first, their size being the old height's: each is built again at the new one at its next use (GetHostSurfaceSlow), as
// a target that grows is. Resolve destinations of the code are made again at their next resolve (textures.cpp, by
// their size). Between two such changes of a code the frame has drawn at the other one, so a resolve texture still of
// the old height and sampled after it has gone two changes without a resolve into it.
RenderScale CodeForHeightLocked(int32_t height) {
  if (height == 720)
    return RenderScale{};
  if (height == 1080)
    return RenderScale{2};
  constexpr uint32_t kCodes[2] = {RenderScale::kHandheldCode, RenderScale::kDockedCode};
  for (int i = 0; i < 2; ++i) {
    if (g_code_height[i] == height) {
      g_code_used[i] = g_latches;
      return RenderScale{kCodes[i]};
    }
  }
  int pick;
  if (g_resolution_scale.code == kCodes[0])
    pick = 1;
  else if (g_resolution_scale.code == kCodes[1])
    pick = 0;
  else
    pick = g_code_used[0] <= g_code_used[1] ? 0 : 1;
  const uint32_t code = kCodes[pick];
  uint32_t retired = 0;
  for (auto &[key, surface] : g_targets) {
    if (surface && surface->texture && surface->scale.code == code) {
      RetireTarget(*surface);
      ++retired;
    }
  }
  const uint32_t g = std::gcd(uint32_t(height), 720u);
  // sasrt-nx (2026-11-12, batch C/D, item C13; port 93): with its factor
  RenderScale::SetRatio(code, uint32_t(height) / g, 720u / g);
  const int32_t old_height = g_code_height[pick];
  g_code_height[pick] = height;
  g_code_used[pick] = g_latches;
  g_internal_heights = pick == 0 ? (g_internal_heights & 0xFFFF0000u) | uint32_t(height)
                                 : (g_internal_heights & 0xFFFFu) | uint32_t(height) << 16;
  ++g_surface_epoch;
  RGPU_INFO("[renut] dynamic resolution: code {} now {} lines ({} targets of its {} lines made again at their next use)",
            code, height, retired, old_height);
  return RenderScale{code};
}
#endif

} // namespace

uint64_t SurfaceEpochLocked() { return g_surface_epoch; }  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_chain_reuse)

// sasrt-nx (2026-11-09; port 58; SASRT: "feel free to improve our memory management ... 4 player mode will probably be
// memory intensive"; its docked race kept 278 MB of render targets after it went back to handheld at 1x, and a test at
// 1.5x in handheld ran the heap out at 35 MB free): a target is keyed by its scale too (TargetKey), so every scale the
// game has drawn at keeps its own set (the 1x one, 1080's, and the settable codes' heights) until its code is
// re-pointed. Now a target of neither the 3D's scale nor the HUD's, not looked up (GetHostSurfaceLocked,
// FindHostSurfaceLocked) for sasrt_retire_idle_targets_frames frames, is released: made again at its next use as a
// re-pointed code's targets are (GetHostSurfaceSlow builds a slot whose texture is gone; what it held then is what a
// scale change gives anyway: the other scale's set never carried the frame's content). Targets the frame uses at another
// scale (post-processing buffers at 1x) are looked up every frame and stay. A released target is no generation's
// (SurfaceByGenerationLocked), so an alias or a restore candidate of it lapses as for any retired one.
REXCVAR_DEFINE_BOOL(sasrt_memory_emergency, true, "SASRT/Graphics",
                    "When plume could not make a texture or a memory block, the next frame frees what it can at once "
                    "(other scales' targets unused for 4 frames, the governor's critical actions for its next three "
                    "looks, dynamic resolution a step down) instead of waiting for the governor's next look")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
std::atomic<bool> g_dynamic_emergency_step{false};
REXCVAR_DEFINE_INT32(sasrt_spare_stacks_idle_s, 60, "SASRT/Graphics",
                     "The guest thread stacks kept for the next threads (switch_thread_stack_pool) go back to the heap "
                     "once no guest thread has been made for this many seconds; 0 keeps them unless memory is low")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sasrt_retire_idle_targets_frames, 300, "SASRT/Graphics",
                     "Render targets of another scale than the 3D's and the HUD's not used for this many frames (300: "
                     "10 s at 30 fps) are released (made again if that scale comes back); 0 keeps them as before")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
uint64_t g_sasrt_frames = 1;

// sasrt-nx (2026-11-09 night; port 58; SASRT's run mem12: at a race's results screen dynamic resolution stepped 972 ->
// 1008 -> 1044 -> 1080 in three seconds; each step made a set of targets and the sets of the steps before stayed for
// sasrt_retire_idle_targets_frames, four sets at once, and the heap went to 35 MB free). Only the 3D's set before its
// last change is kept that long (a step back finds it); a set of any other scale (neither the 3D's, its previous one,
// nor the HUD's) goes after sasrt_retire_stale_frames unused. In an emergency every set but the 3D's and the HUD's goes
// after 4 frames unused.
REXCVAR_DEFINE_BOOL(sasrt_retire_stale_sets, true, "SASRT/Graphics",
                    "Render targets of a scale that is neither the 3D's, the 3D's previous one, nor the HUD's go after "
                    "sasrt_retire_stale_frames unused (sasrt_retire_idle_targets_frames otherwise): at most two 3D sets")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sasrt_retire_stale_frames, 8, "SASRT/Graphics",
                     "sasrt_retire_stale_sets: frames unused before a stale set's target goes (more than the frames in "
                     "flight)")
    .range(4, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sasrt_retire_current_idle_frames, 1800, "SASRT/Graphics",
                     "Render targets of the 3D's or the HUD's own scale unused for this many frames (1800: a minute at "
                     "30 fps) are released too (a menu's or a cutscene's targets during a race; made again at their "
                     "next use); 0 keeps them unless memory is low")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

void RetireIdleTargetsLocked(bool emergency = false) {
  static const int32_t set = REXCVAR_GET(sasrt_retire_idle_targets_frames);
  // every 10 frames, and at once in an emergency
  if (set <= 0 || (!emergency && g_sasrt_frames % 10 != 0))
    return;
  // sooner with memory short (sasrt_memory_governor)
  const int32_t idle = banjo::renut_gpu::MemoryLow() ? std::min(set, 60) : set;
  static const bool stale_sets = REXCVAR_GET(sasrt_retire_stale_sets);
  static const int32_t stale = REXCVAR_GET(sasrt_retire_stale_frames);
  static const int32_t current_idle = REXCVAR_GET(sasrt_retire_current_idle_frames);
  uint32_t count = 0;
  uint64_t bytes = 0, current_bytes = 0;
  // while memory is low, targets of the 3D's or the HUD's own scale go too after 900 frames (30 s) unused (a menu's or a
  // cutscene's targets during a race); made again at their next use.
  const bool low = banjo::renut_gpu::MemoryLow();
  for (auto &[key, surface] : g_targets) {
    if (!surface || !surface->texture)
      continue;
    const uint64_t target_bytes = uint64_t(surface->scale.Size(surface->width)) * surface->scale.Size(surface->height) *
                                  plume::RenderFormatSize(surface->format) * surface->samples;
    if (surface->scale == g_resolution_scale)
      current_bytes += target_bytes;  // sasrt_dynamic_up_memory's figure
    const bool current = surface->scale == g_resolution_scale || surface->scale == g_ui_scale;
    // sasrt_retire_current_idle_frames: the 3D's and the HUD's own scale's targets too after that long unused whatever
    // memory says (900 frames when low)
    if (current && !low && current_idle <= 0)
      continue;
    // sasrt_retire_stale_sets: the 3D's previous set keeps 'idle' (not in an emergency, nor while memory is low); any
    // other set 'stale'
    int32_t limit = current ? (low ? std::max(idle, 900) : current_idle) : idle;
    if (!current && stale_sets && (emergency || low || !(surface->scale == g_prev_resolution_scale)))
      limit = emergency ? 4 : stale;
    if (g_sasrt_frames - surface->sasrt_used_frame < uint64_t(limit))
      continue;
    if (surface.get() == g_ui.scene || surface.get() == g_ui_depth.scene)
      continue;
    bytes += target_bytes;
    RetireTarget(*surface);
    ++count;
  }
  g_current_set_mb.store(uint32_t(current_bytes >> 20), std::memory_order_relaxed);
  static uint32_t logged = 0;
  if (count && logged++ < 64)
    RGPU_INFO("[renut] {} idle render targets released (the 3D at {}x, the HUD at {}x; unused for {} frames, {} for a "
              "stale set{}): {:.1f} MB (sasrt_retire_idle_targets_frames, sasrt_retire_stale_sets)",
              count, g_resolution_scale.Factor(), g_ui_scale.Factor(), idle, emergency ? 4 : stale,
              emergency ? ", an emergency" : "", double(bytes) / 1048576.0);
}
uint64_t FramebufferEpochLocked() { return g_framebuffer_epoch; }  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_chain_reuse)

RenderScale ResolutionScale() {
  return g_resolution_scale;
}

void LatchResolutionScaleLocked() {
  LatchGraphicsOptionsLocked();
  // blur-nx (2026-11-18, the game menu: MSAA at once): the level once a frame; a change makes the MSAA targets anew (their
  // key has it), the old ones retired as idle targets are.
  if (const uint32_t level = banjo::renut_gpu::MsaaLevel(); level != g_msaa_latched) {
    if (g_msaa_latched != 0)
      RGPU_INFO("[renut] MSAA {}x from this frame (msaa_level; the MSAA targets made again)", level);
    g_msaa_latched = level;
    ++g_surface_epoch;
  }
  const std::string resolution = REXCVAR_GET(renut_resolution);
  // Codes of RenderScale.
  RenderScale scale{resolution == "480p"    ? 1u
                    : resolution == "1080p" ? 2u
                    : resolution == "1440p" ? 3u
                    : resolution == "4K"    ? 4u
                                            : 0u};
  RenderScale ui = scale;
  // banjo-nx (2026-10-18): the deko3d renderer drew at the console's resolution only (tools/deko3d_glsl_opt.py, pass
  // scale1). banjo-nx (2026-10-21, internal resolution, the user's request; the docked 1920x1080 swapchain from sa2-nx
  // 1.0.3.1): its package is made without scale1 and fragscale now (with scalecodes: codes 4 and 6 are 9/10 and 5/4,
  // render_scale.h), and the console's mode picks the heights: the display's (720 handheld, banjo_renut_display_height_
  // docked docked) for the HUD and the swapchain, banjo_renut_render_height_handheld / _docked for the 3D image of
  // gameplay frames (those with the final composite within the last kMenuFrames frames). renut_resolution is not taken.
  if (IsDeko3D()) {
    const bool docked = ConsoleDocked();
    const int32_t display = docked ? banjo::renut_gpu::DisplayHeightDocked() : 720;
    const int32_t render =
        docked ? banjo::renut_gpu::RenderHeightDocked() : banjo::renut_gpu::RenderHeightHandheld();
    RenderScale display_scale, render_scale;
    if (!RenderScale::FromRatio(uint32_t(std::max(display, 1)), 720, display_scale) ||
        (display_scale.code != 0 && display_scale.code != 2)) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        RGPU_WARN("banjo_renut_display_height_docked = {} is not 720 or 1080: 720", display);
      }
      display_scale = {};
    }
    // banjo-nx (2026-10-21, package 36z2, the user's request): any height of 360 to 1080 for either mode, read once
    // (the keys are the config's): codes 4 (handheld) and 6 (docked) of render_scale.h get height / 720, rounded to a
    // multiple of 9 so every 16:9 size the game uses (1280x720, 640x360, 320x180) is whole host pixels at it, and the
    // shaders the same heights. 720 is 1x (code 0); 1080 is code 2 (1.5x, the docked display's: no upscale then).
    static const struct Heights {
      int32_t handheld, docked;
      bool dynamic[2];  // banjo-nx (2026-10-28): banjo_renut_dynamic_resolution's range for [handheld, docked]
    } heights = [] {
      auto pick = [](int32_t want, const char *name) {
        const int32_t h = std::clamp((std::clamp(want, 360, 1080) + 4) / 9 * 9, 360, 1080);
        if (h != want)
          RGPU_INFO("banjo_renut_render_height_{} = {}: {} (a multiple of 9 from 360 to 1080)", name, want, h);
        return h;
      };
      int32_t hh = pick(banjo::renut_gpu::RenderHeightHandheld(), "handheld");
      int32_t dh = pick(banjo::renut_gpu::RenderHeightDocked(), "docked");
      // banjo-nx (2026-10-28, GPU block, the user's ranges): each mode's range has 720 (1x, code 0) at one end; the
      // other end takes the mode's code (4 handheld, 6 docked) for the session. 2026-10-28 (night_gpu5, the user's
      // request: docked 720 to 1080): an end of 1080 is code 2 (1.5x, the docked display's own size: the HUD is drawn
      // into the scene without the upscale), as a fixed height of 1080 always was (render_scale below).
      bool dynamic[2] = {false, false};
      if (DynamicResolutionOn()) {
        for (int m = 0; m < 2; ++m) {
          const bool docked_mode = m == 1;
          const int32_t low = pick(DynamicHeight(docked_mode, false), docked_mode ? "dynamic docked low" : "dynamic handheld low");
          const int32_t high = pick(DynamicHeight(docked_mode, true), docked_mode ? "dynamic docked high" : "dynamic handheld high");
          // sasrt-nx (2026-11-09, the user's ranges: handheld 480-720, docked 540-1080; port 52): on deko3d a range need
          // not have 720 at one end (CodeForHeightLocked gives any height a code); a mode then starts at its step nearest
          // below 720 (the 720 end where it has one, as before).
#if defined(PLUME_DEKO3D)
          const bool needs_720 = false;
#else
          const bool needs_720 = true;
#endif
          if (low >= high || (needs_720 && low != 720 && high != 720)) {
            RGPU_WARN("[renut] dynamic resolution, {} mode: {} to {} lines is not a range with 720 at one end: the "
                      "fixed height", docked_mode ? "docked" : "handheld", low, high);
            continue;
          }
          (docked_mode ? dh : hh) = low == 720 ? high : low;
          dynamic[m] = true;
          // banjo-nx (2026-10-28, night_gpu5, banjo_renut_dynamic_steps): the ends and the heights between them, evenly
          // spaced and rounded to a multiple of 36 (whole host pixels at 320x180 too). Deko3d only: the other back ends'
          // codes 4 and 6 are fixed factors (the ends alone there).
          int n = 0;
          g_dynamic_steps[m][n++] = low;
#if defined(PLUME_DEKO3D)
          const int between = DynamicSteps();
          const int32_t grid = DynamicStepLines(docked_mode);  // sasrt-nx (2026-11-09): each mode's grid, 36 before
          for (int i = 1; i <= between && n < kMaxDynamicSteps - 1; ++i) {
            const int32_t h = (low + (high - low) * i / (between + 1) + grid / 2) / grid * grid;
            if (h > g_dynamic_steps[m][n - 1] && h < high)
              g_dynamic_steps[m][n++] = h;
          }
#endif
          g_dynamic_steps[m][n++] = high;
          g_dynamic_count[m] = n;
          std::string list;
          for (int i = 0; i < n; ++i)
            list += fmt::format("{}{}", i ? " " : "", g_dynamic_steps[m][i]);
          RGPU_INFO("[renut] dynamic resolution, {} mode: 3D at {} lines, by the GPU's time a frame against {} ms "
                    "(banjo_renut_dynamic_resolution){}",
                    docked_mode ? "docked" : "handheld", list, DynamicGpuMs(),
                    DynamicTestSeconds() > 0 ? fmt::format("; test: a step every {} s", DynamicTestSeconds())
                                             : std::string());
        }
      }
#if defined(PLUME_DEKO3D)
      auto set = [](uint32_t code, int32_t h) {
        const uint32_t g = std::gcd(uint32_t(h), 720u);
        RenderScale::SetRatio(code, uint32_t(h) / g, 720u / g);  // sasrt-nx (2026-11-12, batch C/D, item C13; port 93)
      };
      set(RenderScale::kHandheldCode, hh);
      set(RenderScale::kDockedCode, dh);
      g_code_height[0] = hh;  // banjo-nx (2026-10-28, night_gpu5): CodeForHeightLocked's
      g_code_height[1] = dh;
#endif
      g_internal_heights = uint32_t(hh) | uint32_t(dh) << 16;
      RGPU_INFO("Internal heights: handheld {}x{}, docked {}x{}", RenderScale{RenderScale::kHandheldCode}.Size(1280),
                hh, RenderScale{RenderScale::kDockedCode}.Size(1280), dh);
      return Heights{hh, dh, {dynamic[0], dynamic[1]}};
    }();
    // banjo-nx (2026-10-28, GPU block, banjo_renut_dynamic_resolution): the mode's range by the GPU's time; a change of
    // mode starts it at its 720 end (night_gpu2 console run: docking at 900 made the docked set of targets with the heap
    // at its end; 720 is the smaller set, at the 1x code both modes use).
    const int mode = docked ? 1 : 0;
    const int count = heights.dynamic[mode] ? g_dynamic_count[mode] : 0;
    static bool dynamic_started = false;
    const bool mode_changed = g_dynamic_docked.exchange(docked) != docked;
    if ((mode_changed || !dynamic_started) && count) {
      dynamic_started = true;
      // the 720 end, or (sasrt-nx, 2026-11-09; port 52) the step nearest below 720 for a range without it
      int start = 0;
      for (int i = 0; i < count; ++i)
        if (g_dynamic_steps[mode][i] <= 720)
          start = i;
      g_dynamic_step = start;
    }
    ++g_latches;
#if defined(PLUME_DEKO3D)
    // blur-nx (2026-11-18, the game menu: the resolutions at once): the configured heights read at every latch, rounded
    // as at start; CodeForHeightLocked gives a new one a code (the dynamic resolution's way), its targets made anew.
    auto rounded = [](int32_t want) { return std::clamp((std::clamp(want, 360, 1080) + 4) / 9 * 9, 360, 1080); };
    const int32_t configured = docked ? rounded(banjo::renut_gpu::RenderHeightDocked())
                                      : rounded(banjo::renut_gpu::RenderHeightHandheld());
#else
    const int32_t configured = docked ? heights.docked : heights.handheld;
#endif
    const int32_t chosen = count ? g_dynamic_steps[mode][std::clamp(g_dynamic_step.load(), 0, count - 1)] : configured;
#if defined(PLUME_DEKO3D)
    render_scale = CodeForHeightLocked(chosen);  // banjo-nx (2026-10-28, night_gpu5): the steps take turns on codes 4 and 6
#else
    render_scale = chosen == 720    ? RenderScale{}
                   : chosen == 1080 ? RenderScale{2}
                                    : RenderScale{docked ? RenderScale::kDockedCode : RenderScale::kHandheldCode};
#endif
    (void)render;
    g_frames_without_composite = g_composite_seen ? 0 : std::min(g_frames_without_composite + 1, kMenuFrames);
    g_composite_seen = false;
    ui = display_scale;
    scale = g_frames_without_composite < kMenuFrames ? render_scale : display_scale;
#if defined(PLUME_DEKO3D)
    // The swapchain at the display's size (from sa2-nx 1.0.3.1): the present scales the frame to it.
    static int32_t last_display = 0;
    if (last_display != display) {
      last_display = display;
      plume::Deko3DSetSwapChainSize(display == 1080 ? 1920u : 1280u, display == 1080 ? 1080u : 720u);
      RGPU_INFO("[renut] {} mode: the swapchain at {}", docked ? "docked" : "handheld",
                display == 1080 ? "1920x1080" : "1280x720");
    }
#endif
  }
  // The frame's redirect to the UI target ends with it.
  for (UiTarget *u : {&g_ui, &g_ui_depth}) {
    if (u->redirect) {
      u->redirect = false;
      ++g_surface_epoch;
    }
  }
  if (ui != g_ui_scale) {
    g_ui_scale = ui;
    ++g_surface_epoch;
    RGPU_INFO("HUD resolution {}x{} ({:.3g}x)", ui.Size(GuestScreenWidth()), ui.Size(720), ui.Factor());
  }
  if (!g_resolution_latched || scale != g_resolution_scale) {
    RGPU_INFO("Render resolution {}x{} ({:.3g}x)", scale.Size(GuestScreenWidth()), scale.Size(720),
              scale.Factor());
    if (g_resolution_latched)
      g_prev_resolution_scale = g_resolution_scale;  // sasrt-nx (2026-11-09 night; port 58)
    g_resolution_scale = scale;
    ++g_surface_epoch;
    g_resolution_latched = true;
  }
  ++g_sasrt_frames;  // sasrt-nx (2026-11-09; port 58): once a frame (the Swap's latch)
#if defined(PLUME_DEKO3D)
  // sasrt-nx (2026-11-09 night, sasrt_memory_emergency; port 58): a texture or a block plume could not make since the
  // last frame
  static const bool emergencies = REXCVAR_GET(sasrt_memory_emergency);
  const bool emergency = plume::Deko3DTakeMemoryEmergency() && emergencies;
  if (emergency) {
    banjo::renut_gpu::g_memory_emergency_looks.store(3, std::memory_order_relaxed);
    g_dynamic_emergency_step.store(true, std::memory_order_relaxed);
    RetireIdleTargetsLocked(true);
    static uint32_t emergency_logged = 0;
    if (emergency_logged++ < 32)
      RGPU_INFO("[renut] memory emergency (a texture or a memory block could not be made): sets of other scales unused "
                "for 4 frames released, dynamic resolution a step down, the governor's critical actions now "
                "(sasrt_memory_emergency)");
  }
  // sasrt-nx (2026-11-09, sasrt_memory_governor; port 58): every 10 frames (its figures are O(1)), and at once in an
  // emergency
  if (emergency || g_sasrt_frames % 10 == 1) {
    // What a GPU block or a CPU allocation can still have: the heap's free memory and the GPU region's (carved from the
    // heap's top: SASRT's run docked_gov held 834 MB free in it while the heap alone read 105 MB); the heap alone when
    // it is nearly gone (the CPU side needs it).
    plume::Deko3DGpuRegionInfo region{};
    plume::Deko3DGpuRegionFigures(region);
    // the heap's room (what sbrk can still give), O(1): Deko3DHeapFreeBytes adds malloc's free pieces through mallinfo,
    // 7-21 ms under malloc's lock in SASRT's run mem12 (pieces of a few KB that a block or a big allocation cannot use)
    const uint64_t heap = plume::Deko3DHeapRoomBytes();
    uint64_t room = heap + (region.on ? uint64_t(region.freeBytes) : 0);
    {  // sasrt-nx (2026-11-11, sasrt_target_block_cache_mb): the kept target blocks are given back at the low mark
      plume::Deko3DTargetCacheInfo cache;
      plume::Deko3DTargetCacheFigures(cache);
      room += cache.bytes;
    }
    if ((heap >> 20) < 48)
      room = std::min(room, heap);
    const uint32_t mb = uint32_t(std::min<uint64_t>(room >> 20, 0xFFFFFFF0ull));
    const bool was_low = banjo::renut_gpu::MemoryLow(), was_critical = banjo::renut_gpu::MemoryCritical();
    banjo::renut_gpu::g_heap_free_mb.store(mb, std::memory_order_relaxed);
    const bool low = banjo::renut_gpu::MemoryLow(), critical = banjo::renut_gpu::MemoryCritical();
    if (!emergency && banjo::renut_gpu::g_memory_emergency_looks.load(std::memory_order_relaxed))
      banjo::renut_gpu::g_memory_emergency_looks.fetch_sub(1, std::memory_order_relaxed);
    // the GPU region's free bottom back to the heap while the heap itself (the CPU side's) is short: SASRT's docked race
    // crashed with the command buffers' arena out of heap and 500-700 MB free in the region
    if ((heap >> 20) < 192) {
      const uint64_t given = plume::Deko3DRegionGiveBack();
      static uint32_t given_logged = 0;
      if (given && given_logged++ < 32)
        RGPU_INFO("[renut] heap {} MB free: {} MB of the GPU region's free bottom given back to it "
                  "(sasrt_memory_governor)", heap >> 20, given >> 20);
    }
    if (low) {
      // the I/O range cache's entries (heap; a range is read again)
      const uint64_t dropped = rex::filesystem::TrimRangeCache(0);
      static uint32_t dropped_logged = 0;
      if (dropped && dropped_logged++ < 32)
        RGPU_INFO("[renut] {} MB free: the I/O range cache's {} MB dropped (sasrt_memory_governor)", mb,
                  dropped >> 20);
      // the image heap's empty chunks back to the GPU region (for buffers too)
      const uint64_t freed = plume::Deko3DReleaseEmptyImageChunks();
      static uint32_t freed_logged = 0;
      if (freed && freed_logged++ < 32)
        RGPU_INFO("[renut] {} MB free: {} MB of empty image chunks given back (sasrt_memory_governor)", mb,
                  freed >> 20);
#if defined(__SWITCH__)
      // the guest thread stacks kept for the next threads (up to switch_thread_stack_pool of them)
      const size_t stacks = RexSwitchReleaseSpareStacks();
      static uint32_t stacks_logged = 0;
      if (stacks && stacks_logged++ < 32)
        RGPU_INFO("[renut] {} MB free: {} MB of kept thread stacks given back to the heap (sasrt_memory_governor)", mb,
                  stacks >> 20);
#endif
    }
#if defined(__SWITCH__)
    // sasrt-nx (2026-11-09 night, sasrt_spare_stacks_idle_s; port 58): the kept guest thread stacks back to the heap once
    // no thread has been made for that long (SASRT's race makes none), whatever memory says
    {
      static const int32_t idle_s = REXCVAR_GET(sasrt_spare_stacks_idle_s);
      static uint64_t last_taken = UINT64_MAX;
      static std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
      const uint64_t taken = RexSwitchStacksTaken();
      const auto now = std::chrono::steady_clock::now();
      if (taken != last_taken) {
        last_taken = taken;
        since = now;
      } else if (idle_s > 0 && now - since >= std::chrono::seconds(idle_s)) {
        since = now;
        const size_t given = RexSwitchReleaseSpareStacks();
        static uint32_t idle_logged = 0;
        if (given && idle_logged++ < 16)
          RGPU_INFO("[renut] no guest thread made for {} s: {} MB of kept thread stacks given back to the heap "
                    "(sasrt_spare_stacks_idle_s)", idle_s, given >> 20);
      }
    }
#endif
    static uint32_t logged = 0;
    if ((low != was_low || critical != was_critical) && logged++ < 64)
      RGPU_INFO("[renut] {} MB free (heap and GPU region): {} (sasrt_memory_governor)", mb,
                critical ? "critical: dynamic resolution steps down, the stream cache at 48 MB"
                : low    ? "low: no dynamic resolution step up, the stream cache at 96 MB, idle targets after 60 frames"
                         : "enough again");
  }
#endif
  RetireIdleTargetsLocked();
}

// banjo-nx (2026-10-28, GPU block): see render/surfaces.h. A moving average of the GPU's time a frame (about 16
// frames); down above the budget, up when the next height's predicted time (the current one times the pixel ratio, an
// overestimate) has been under 85 % of it for banjo_renut_dynamic_up_s in a row (and that long since the last change);
// down 5 s at least after a change. night_gpu5 (the user's request, banjo_renut_dynamic_steps): with heights between the
// ends, down goes to the highest step predicted to fit the budget (the lowest if none) and up to the highest predicted
// under 85 % of it, in one change each.
// sasrt-nx (2026-11-09, the user: "we were quite CPU limited"; their handheld race: render thread 33-50 ms a frame,
// GPU 28-30 ms at 612 lines, 37 at 720; port 52): dynamic resolution gives up lines only where the GPU is what holds the
// frame.
REXCVAR_DEFINE_BOOL(sasrt_dynamic_cpu_aware, true, "Nuts&Bolts/Graphics",
                    "banjo_renut_dynamic_resolution's budget (banjo_renut_dynamic_gpu_ms) raised to the CPU side's time a "
                    "frame when that is longer (the render thread's busy time or the game thread's, the larger: moving "
                    "averages): a frame the CPU holds is not made faster by fewer pixels, so the height steps down only "
                    "when the GPU's time is above both, and up while the next step is predicted under 85 % of the larger "
                    "(from sasrt-nx). Off: the budget alone, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// sasrt-nx (2026-11-09, the user: "raise it if there's 1.0 ms of room for 1 second"): the next step is taken when its
// predicted time is at least this far under the budget for banjo_renut_dynamic_up_s in a row (in place of "under 85 %
// of the budget": 29.3 ms at 34.5, which held the handheld race at 612 lines with the GPU at 27-30 ms there). 0: the
// 85 % rule. The prediction (the time now times the pixel ratio) is an overestimate, so a step taken by it fits.
// vivapinata-nx (2026-11-12, the owner: "copy sonic racing it fixed the issue"): run c28 held 576 lines at 26.96 ms GPU
// against a 32 ms budget (612 predicted 30.4 ms, over the 85 % rule's 27.2; under this rule's 31.0).
REXCVAR_DEFINE_DOUBLE(sasrt_dynamic_up_room_ms, 1.0, "Nuts&Bolts/Graphics",
                      "banjo_renut_dynamic_resolution: step up when the next height's predicted GPU time a frame has "
                      "been this many ms under the budget for banjo_renut_dynamic_up_s (0: under 85 % of the budget; "
                      "from sasrt-nx)");

// sasrt-nx (2026-11-09, the user: "more steps, especially in docked"; console run steps_race, handheld 540-720 every
// 18 lines): a step up went to the highest step predicted with room, often two at once (666 -> 702), onto a height
// neither settable code held: 14 targets made again each time (17 such changes in the race against 2 with 6 steps).
// One step at a time, the moves stay between the heights the codes hold (and 720, code 0) unless the band moves.
REXCVAR_DEFINE_BOOL(sasrt_dynamic_up_one_step, true, "Nuts&Bolts/Graphics",
                    "banjo_renut_dynamic_resolution: a step up goes one step (when the next has the room), not to the "
                    "highest step with room: fewer changes of a settable code's height (each makes its targets again; "
                    "from sasrt-nx)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx (2026-11-09 night, SASRT's run mem12; port 58): see the step-up branch below.
REXCVAR_DEFINE_BOOL(sasrt_dynamic_up_memory, true, "SASRT/Graphics",
                    "banjo_renut_dynamic_resolution: a step up only when the heap and the GPU region have its set of "
                    "targets free above sasrt_memory_low_mb (the 3D's set now times the pixel ratio)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

void NoteGpuFrameForDynamicResolution(double gpu_ms) {
  const int mode = g_dynamic_docked ? 1 : 0;
  const int count = g_dynamic_count[mode];
  if (count < 2 || gpu_ms <= 0.0)
    return;
  // banjo-nx (2026-10-28): up after banjo_renut_dynamic_up_s in a row of fitting, 1 s by default (the user: "be
  // aggressive"). h9 had 20 s, after a loading screen's light frames took the city's 540 to 720 for 12 s (each change
  // makes a set of targets); the h10 GPU region is what keeps that memory in check now.
  const double up_seconds = DynamicUpSeconds();
  static double average = 0.0;
  static std::chrono::steady_clock::time_point last_change = std::chrono::steady_clock::now();
  static std::chrono::steady_clock::time_point fits_since = last_change;
  static int last_mode = mode;
  average = average > 0.0 ? average * 0.94 + gpu_ms * 0.06 : gpu_ms;
  const auto now = std::chrono::steady_clock::now();
  if (mode != last_mode) {  // a change of mode starts again at its 720 end (see above): its own hold
    last_mode = mode;
    fits_since = now;
  }
  const double since = std::chrono::duration<double>(now - last_change).count();
  const int step = std::clamp(g_dynamic_step.load(), 0, count - 1);
  const int32_t *heights = g_dynamic_steps[mode];
  const double current = double(heights[step]);
  double budget = DynamicGpuMs();
  // sasrt-nx (2026-11-09, sasrt_dynamic_cpu_aware; port 52): the CPU side's time a frame, when longer, as the budget.
  static const bool cpu_aware = REXCVAR_GET(sasrt_dynamic_cpu_aware);
  if (cpu_aware) {
    static double cpu_average = 0.0;
    const frame_stats::Snapshot fs = frame_stats::Get();
    const double cpu_side = std::max(fs.render_thread_known ? fs.render_thread_ms : 0.0, fs.cpu_ms);
    if (cpu_side > 0.0)
      cpu_average = cpu_average > 0.0 ? cpu_average * 0.94 + cpu_side * 0.06 : cpu_side;
    budget = std::max(budget, cpu_average);
  }
  // sasrt-nx (2026-11-09, sasrt_dynamic_up_room_ms): the predicted time a step up must stay at or under.
  static const double up_room = REXCVAR_GET(sasrt_dynamic_up_room_ms);
  const double up_limit = up_room > 0.0 ? budget - up_room : budget * 0.85;
  // The time a step is predicted to take: the average times the pixel ratio.
  const auto predicted = [&](int s) {
    const double r = double(heights[s]) / current;
    return average * r * r;
  };
  int next = step;
  const int32_t test = DynamicTestSeconds();
  if (test > 0) {
    // The test walks the steps one at a time, up to an end and back.
    static int direction = -1;
    if (since >= double(test)) {
      if (step + direction < 0 || step + direction >= count)
        direction = -direction;
      next = step + direction;
    }
  } else if (step > 0 && banjo::renut_gpu::MemoryCritical() &&
             (since >= 2.0 || g_dynamic_emergency_step.exchange(false, std::memory_order_relaxed))) {
    next = step - 1;  // sasrt-nx (2026-11-09, sasrt_memory_governor; port 58): a smaller set of targets for the heap
  } else if (step > 0 && since >= 5.0 && average > budget) {
    next = 0;
    for (int s = step - 1; s >= 0; --s) {
      if (predicted(s) <= budget) {
        next = s;
        break;
      }
    }
  } else if (step + 1 < count) {
    // sasrt-nx (2026-11-09 night, sasrt_dynamic_up_memory; port 58): the next step's set of targets (the 3D's set now,
    // times the pixel ratio: an overestimate, its codes' targets of other heights go) must leave the low mark free
    static const bool up_memory = REXCVAR_GET(sasrt_dynamic_up_memory);
    bool memory_ok = true;
    if (up_memory) {
      const uint32_t free_mb = banjo::renut_gpu::g_heap_free_mb.load(std::memory_order_relaxed);
      const double r = double(heights[step + 1]) / current;
      const double need_mb = double(g_current_set_mb.load(std::memory_order_relaxed)) * r * r;
      memory_ok = free_mb == UINT32_MAX || double(free_mb) >= double(banjo::renut_gpu::MemoryLowMarkMb()) + need_mb;
      static uint32_t held_logged = 0;
      if (!memory_ok && predicted(step + 1) <= up_limit && held_logged++ < 16)
        RGPU_INFO("[renut] dynamic resolution: {} lines held back: {} MB free, the next set ~{:.0f} MB and the low mark "
                  "{} MB (sasrt_dynamic_up_memory)", heights[step + 1], free_mb, need_mb,
                  banjo::renut_gpu::MemoryLowMarkMb());
    }
    if (predicted(step + 1) > up_limit || banjo::renut_gpu::MemoryLow() || !memory_ok)  // port 58: the governor
      fits_since = now;
    if (since >= up_seconds && std::chrono::duration<double>(now - fits_since).count() >= up_seconds) {
      next = step + 1;
      static const bool one_step = REXCVAR_GET(sasrt_dynamic_up_one_step);  // sasrt-nx (2026-11-09)
      while (!one_step && next + 1 < count && predicted(next + 1) <= up_limit)
        ++next;
    }
  }
  if (next == step)
    return;
  const double was = average;
  average = predicted(next);  // the frames to come are at the new height
  g_dynamic_step = next;
  last_change = now;
  fits_since = now;
  RGPU_INFO("[renut] dynamic resolution: 3D at {} lines (the GPU's time a frame {:.1f} ms on average at {}; "
            "banjo_renut_dynamic_gpu_ms {}, the budget {:.1f} ms with the CPU side's time: sasrt_dynamic_cpu_aware)",
            heights[next], was, heights[step], DynamicGpuMs(), budget);
}

void SetTilingExtentLocked(uint32_t width, uint32_t height) {
  if (g_tiling_width != width || g_tiling_height != height)
    ++g_surface_epoch;
  g_tiling_width = width;
  g_tiling_height = height;
}

void DropTarget(uint64_t key) {
  if (auto it = g_targets.find(key); it != g_targets.end()) {
    RetireTarget(*it->second);
    g_targets.erase(it);
  }
}

// Port 145 (banjo_renut_keep_unused_targets): kept targets unused for kUnusedTargetMs, and the oldest beyond
// banjo_renut_kept_targets, go (tw2de_nx: 250 ms and 24; the views a game switches between come back within a frame,
// and nothing is kept long enough to matter for memory). Swept every frame too (SweepUnusedTargetsLocked).
constexpr int64_t kUnusedTargetMs = 250;
int64_t TargetNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void SweepUnusedTargets() {
  if (g_unused_targets.empty())
    return;
  const int64_t now = TargetNowMs();
  for (auto it = g_unused_targets.begin(); it != g_unused_targets.end();) {
    if (now - it->second > kUnusedTargetMs) {
      DropTarget(it->first);
      it = g_unused_targets.erase(it);
    } else {
      ++it;
    }
  }
  const size_t kept_max = size_t(std::max(1, int(REXCVAR_GET(banjo_renut_kept_targets))));
  while (g_unused_targets.size() > kept_max) {
    auto oldest = std::min_element(g_unused_targets.begin(), g_unused_targets.end(),
                                   [](const auto &a, const auto &b) { return a.second < b.second; });
    DropTarget(oldest->first);
    g_unused_targets.erase(oldest);
  }
}

// A target gets a user again: no longer a kept one.
void UseTarget(uint64_t key) {
  if (++g_target_users[key] == 1 && !g_unused_targets.empty())
    g_unused_targets.erase(key);
}

// A surface stops using its target (destroyed, or its header now describes
// another view); the last user retires it (port 145: or keeps it, banjo_renut_keep_unused_targets).
void UnuseTarget(uint64_t key) {
  auto users = g_target_users.find(key);
  if (users == g_target_users.end() || --users->second)
    return;
  g_target_users.erase(users);
  ++g_surface_epoch;
  ++g_sasrt_table_epoch;  // sasrt-nx (2026-11-09, sasrt_rt_restore_candidates; port 54)
  static const bool keep = REXCVAR_GET(banjo_renut_keep_unused_targets);
  if (keep && g_targets.count(key)) {
    g_unused_targets[key] = TargetNowMs();
    SweepUnusedTargets();
    return;
  }
  DropTarget(key);
}

void SweepUnusedTargetsLocked() { SweepUnusedTargets(); }

HostSurface *FindHostSurfaceLocked(u32 surface_va) {
  auto key = g_surface_keys.find(surface_va);
  if (key == g_surface_keys.end())
    return nullptr;
  auto it = g_targets.find(key->second);
  if (it != g_targets.end() && it->second)
    it->second->sasrt_used_frame = g_sasrt_frames;  // sasrt-nx (2026-11-09, sasrt_retire_idle_targets_frames; port 58)
  return it != g_targets.end() ? it->second.get() : nullptr;
}

void ColorAliasesLocked(const HostSurface *surface, std::vector<HostSurface *> &out) {
  out.clear();
  if (!surface || surface->depth)
    return;
  // TargetKey: the view in the low 32 bits, the host format above.
  constexpr uint64_t kViewMask = 0xFFFFFFFFull;
  uint64_t own = 0;
  bool found = false;
  for (const auto &[key, target] : g_targets) {
    if (target.get() == surface) {
      own = key;
      found = true;
      break;
    }
  }
  if (!found)
    return;
  for (const auto &[key, target] : g_targets) {
    if (target && target.get() != surface && target->texture && !target->depth &&
        (key & kViewMask) == (own & kViewMask))
      out.push_back(target.get());
  }
}

// blur-nx (2026-11-10, port 76; its keys 218 and 220): the EDRAM tiles of a colour target, exactly. A tile is 80x16 samples
// of 32 bits (40x16 of the 64-bit formats: 16_16_16_16, 16_16_16_16_FLOAT, 32_32_FLOAT); a pixel's samples are 2x2 at 4x
// and 1x2 at 2x (the SDK's EDRAM layout); a surface's tiles run row by row from its base tile, 'columns' a row (its
// pitch's), modulo the 2048 tiles.
struct EdramSpan {
  u32 base = 0, columns = 0, rows = 0, samples_x = 1, samples_y = 1, tile_samples_x = 80;
  u32 count() const { return columns * rows; }
};
EdramSpan EdramSpanOf(const HostSurface *s) {
  EdramSpan e;
  const u32 info = s->info_word, msaa = (info >> 16) & 3;
  e.samples_x = msaa >= 2 ? 2 : 1;
  e.samples_y = msaa >= 1 ? 2 : 1;
  const u32 format = (u32(s->color_info_word) >> 16) & 0xF;
  e.tile_samples_x = (format == 5 || format == 7 || format == 15) ? 40 : 80;
  e.base = u32(s->color_info_word) & 0xFFF;
  e.columns = ((info & 0x3FFF) * e.samples_x + e.tile_samples_x - 1) / e.tile_samples_x;
  e.rows = (s->height * e.samples_y + 15) / 16;
  return e;
}
bool EdramSpansIntersect(const EdramSpan &a, const EdramSpan &b) {
  return a.count() && b.count() && (((b.base - a.base) & 2047u) < a.count() || ((a.base - b.base) & 2047u) < b.count());
}

// vivapinata-nx (2026-11-04, e242c74b; see surfaces.h).
void ColorEdramWipedLocked(const HostSurface *surface, const plume::RenderRect &rect, std::vector<HostSurface *> &out) {
  out.clear();
  if (!surface || surface->depth)
    return;
  // TargetKey: the base tile in the low 12 bits, the samples at bit 26 (0: 1, 1: 2, 2: 4), the view in the low 32 bits.
  uint64_t own = 0;
  bool found = false;
  for (const auto &[key, target] : g_targets) {
    if (target.get() == surface) {
      own = key;
      found = true;
      break;
    }
  }
  if (!found)
    return;
  // blur-nx (2026-11-10, port 76; its key 218: blur_renut_wipe_overwritten): exactly: a target is wiped when every one of
  // its tiles is a tile the clear fills whole (the tile rows and columns of the rect, its partly covered edge tiles left
  // out), whatever its base (below, off: the same base tile, and a tile count no greater than the clear's).
  if (banjo::renut_gpu::WipeOverwrittenOn()) {
    const EdramSpan c = EdramSpanOf(surface);
    if (!c.columns)
      return;
    // The tiles the rect fills whole: columns c0..c1, rows r0..r1 of the clearing surface's span.
    const u32 left = u32(std::max(rect.left, 0)) * c.samples_x, right = u32(std::max(rect.right, 0)) * c.samples_x;
    const u32 top = u32(std::max(rect.top, 0)) * c.samples_y, bottom = u32(std::max(rect.bottom, 0)) * c.samples_y;
    const u32 c0 = (left + c.tile_samples_x - 1) / c.tile_samples_x, c1 = std::min(right / c.tile_samples_x, c.columns);
    const u32 r0 = (top + 15) / 16, r1 = std::min(bottom / 16, c.rows);
    if (c1 <= c0 || r1 <= r0)
      return;
    const auto filled = [&](u32 tile) {
      const u32 offset = (tile - c.base) & 2047u;
      const u32 row = offset / c.columns, column = offset % c.columns;
      return row >= r0 && row < r1 && column >= c0 && column < c1;
    };
    constexpr uint64_t kOwnViewMask = 0xFFFFFFFFull;
    for (const auto &[key, target] : g_targets) {
      if (!target || target.get() == surface || !target->texture || target->depth ||
          (key & kOwnViewMask) == (own & kOwnViewMask))
        continue;
      const EdramSpan t = EdramSpanOf(target.get());
      if (!t.count() || !EdramSpansIntersect(c, t))
        continue;
      bool all = true;
      for (u32 i = 0; i < t.count() && all; ++i)
        all = filled((t.base + i) & 2047u);
      if (all)
        out.push_back(target.get());
    }
    return;
  }
  // A tile is 80x16 samples: at 2 samples a pixel is 80x8 pixels, at 4 samples 40x8. Whole tiles of the clear, at 32 bits a sample:
  // a 64-bit target takes twice the tiles, and is left alone unless these cover it as 32-bit ones would.
  const auto tile_columns = [](int32_t width, uint64_t key) {
    return uint32_t(width) * (((key >> 26) & 3) >= 2 ? 2u : 1u) / 80u;
  };
  const auto tile_rows = [](int32_t height, uint64_t key) {
    return uint32_t(height) * (((key >> 26) & 3) >= 1 ? 2u : 1u) / 16u;
  };
  const uint32_t cleared = tile_columns(rect.right - rect.left, own) * tile_rows(rect.bottom - rect.top, own);
  constexpr uint64_t kViewMask = 0xFFFFFFFFull;
  for (const auto &[key, target] : g_targets) {
    if (!target || target.get() == surface || !target->texture || target->depth || (key & 0xFFF) != (own & 0xFFF) ||
        (key & kViewMask) == (own & kViewMask))
      continue;
    const uint32_t columns = (uint32_t(target->width) * (((key >> 26) & 3) >= 2 ? 2u : 1u) + 79u) / 80u;
    const uint32_t rows = (uint32_t(target->height) * (((key >> 26) & 3) >= 1 ? 2u : 1u) + 15u) / 16u;
    if (columns * rows <= cleared)
      out.push_back(target.get());
  }
}

HostSurface *GetHostSurfaceSlow(u32 surface_va, const d3d::SurfaceDesc *desc);

// blur-nx (2026-11-10, port 76; its key 215: blur_renut_stack_surface_headers): headers of surfaces on the stack, as the
// game bound them (NoteSurfaceHeader, game threads); the render thread reads a surface's header when it replays, by
// which time a temporary D3DSurface on the stack has been overwritten. Blur builds one for the clear and resolve that
// put its 640x360 motion-blur source in place: read later it was zeros (an empty 1x1 surface), and the frame lost its
// motion blur. The operation's own copy (CaptureStackHeaders) comes first; this table is a counted fallback.
std::mutex g_stack_headers_mutex;
std::unordered_map<u32, d3d::SurfaceDesc> g_stack_headers;
inline bool StackVa(u32 va) { return va >= 0x70000000u && va < 0x80000000u; }

// Port 174 (from rehatched-nx, its commit e0794e0b of 2026-11-16; banjo_renut_all_surface_headers): Angry Birds Trilogy
// re-points one heap header between 1280-, 640- and 320-pitch views of EDRAM several times a frame; the device holds
// only the address, the same for each. The render thread read the header when it replayed a draw, by which time the
// game had often re-pointed it (its sprites and the bloom downsample went to the wrong view). The console's GPU takes a
// surface's layout when it is bound (SetRenderTarget / SetDepthStencilSurface write RB_SURFACE_INFO and RB_COLOR_INFO).
// With the key on, every bound surface's header is kept as it was at its bind (NoteSurfaceHeader) and captured with each
// operation (CaptureStackHeaders), not only the stack's (blur_renut_stack_surface_headers). Off: as before.
bool AllSurfaceHeadersOn() {
  static const bool on = REXCVAR_GET(banjo_renut_all_surface_headers);
  return on;
}
inline bool CapturedHeaderVa(u32 va) { return AllSurfaceHeadersOn() || StackVa(va); }

// blur-nx (2026-11-10, port 76; its key 218: blur_renut_wipe_overwritten; see surfaces.h).
bool OverlapWrittenSinceLocked(const HostSurface *surface, uint64_t seq) {
  if (!surface || surface->depth)
    return false;
  bool found = false;
  for (const auto &[key, target] : g_targets) {
    (void)key;
    if (target.get() == surface) {
      found = true;
      break;
    }
  }
  if (!found)
    return false;
  // Any other colour target whose EDRAM tiles meet this one's (EdramSpanOf), written after 'seq'.
  const EdramSpan mine = EdramSpanOf(surface);
  for (const auto &[key, target] : g_targets) {
    (void)key;
    if (target && target.get() != surface && target->texture && !target->depth && target->write_seq > seq &&
        EdramSpansIntersect(mine, EdramSpanOf(target.get())))
      return true;
  }
  return false;
}

// blur-nx (2026-11-11, port 76; its key 220: blur_renut_edram_inherit; see surfaces.h).
bool EdramInheritSpanLocked(const HostSurface *surface, u32 &bits, u32 &rows) {
  if (!surface || surface->depth)
    return false;
  const EdramSpan e = EdramSpanOf(surface);
  // More than the EDRAM's 2048 tiles: a predicated-tiling target backing every tile (its layout wraps; not one view).
  if (!e.count() || e.count() > 2048 || e.columns > 0xFFF)
    return false;
  bits = (e.base & 0xFFFu) | (e.columns << 12) | ((e.samples_x == 2 ? 1u : 0u) << 24) |
         ((e.samples_y == 2 ? 1u : 0u) << 25) | ((e.tile_samples_x == 40 ? 1u : 0u) << 26);
  rows = e.rows;
  return true;
}

// blur-nx (2026-11-11, port 76; its key 220: blur_renut_edram_inherit; see surfaces.h).
void EdramInheritSourcesLocked(const HostSurface *target, std::vector<HostSurface *> &out, u32 &skipped) {
  out.clear();
  skipped = 0;
  if (!target || target->depth || !target->texture)
    return;
  const EdramSpan mine = EdramSpanOf(target);
  if (!mine.count() || mine.count() > 2048)
    return;  // a predicated-tiling target backing every tile (EdramInheritSpanLocked)
  const u32 format = (u32(target->color_info_word) >> 16) & 0xF;
  for (const auto &[key, other] : g_targets) {
    (void)key;
    if (!other || other.get() == target || !other->texture || other->depth || other->write_seq <= target->write_seq)
      continue;
    const EdramSpan theirs = EdramSpanOf(other.get());
    if (!EdramSpansIntersect(mine, theirs))
      continue;
    if (theirs.count() > 2048) {
      ++skipped;  // a predicated-tiling target backing every tile: its samples have no single place in the EDRAM
      continue;
    }
    // The same EDRAM format (so the same word means the same value, whatever each host target's precision) and both
    // at the guest's scale (the console's layout is in guest samples).
    if (((u32(other->color_info_word) >> 16) & 0xF) != format || theirs.tile_samples_x != mine.tile_samples_x ||
        other->scale.code != 0 || target->scale.code != 0) {
      ++skipped;
      continue;
    }
    out.push_back(other.get());
  }
  // Newest first, each keeps the target's tiles no newer one already covers; one left with none is dropped (on the
  // console a later write over the same tiles is what they hold). Then oldest first, the order they are drawn in.
  std::sort(out.begin(), out.end(), [](const HostSurface *a, const HostSurface *b) { return a->write_seq > b->write_seq; });
  static std::vector<uint8_t> covered;
  covered.assign(mine.count(), 0);
  size_t kept = 0;
  for (HostSurface *source : out) {
    const EdramSpan theirs = EdramSpanOf(source);
    bool adds = false;
    for (u32 j = 0; j < mine.count(); ++j) {
      if (covered[j] || ((((mine.base + j) & 2047u) - theirs.base) & 2047u) >= theirs.count())
        continue;
      covered[j] = 1;
      adds = true;
    }
    if (adds)
      out[kept++] = source;
  }
  out.resize(kept);
  std::reverse(out.begin(), out.end());
}

// blur-nx (2026-11-10, port 76; its key 216: blur_renut_sample_grid_resolve; see surfaces.h).
HostSurface *SampleGridAliasLocked(const HostSurface *s) {
  if (!s || s->depth)
    return nullptr;
  // The single-sampled target of the view's sample grid: 4x is 2x2 samples a pixel (twice the pitch), 2x is 1x2 (the
  // same pitch; its rows are the samples' rows).
  // blur-nx (2026-11-13, MSAA lowered): the console's sample count (the header's), not the host target's: msaa_level
  // may make a 4x view single-sampled here, and the trick is the console's EDRAM layout, which stays 4x.
  u32 across = 0;
  const plume::RenderSampleCounts guest = SampleCount(s->info_word);
  if (guest == plume::RenderSampleCount::COUNT_4)
    across = 2;
  else if (guest == plume::RenderSampleCount::COUNT_2)
    across = 1;
  else
    return nullptr;
  const u32 pitch = u32(s->info_word) & 0x3FFF;
  if (!pitch || pitch * across > 0x3FFF)
    return nullptr;
  // TargetKey's layout: the base tile, the pitch, the samples (0: 1), not depth, the scale code, the host format.
  const uint64_t key = uint64_t(u32(s->color_info_word) & 0xFFF) | uint64_t(pitch * across) << 12 |
                       uint64_t(s->scale.code & 7) << 29 | uint64_t(u32(s->format)) << 32;
  const auto it = g_targets.find(key);
  if (it == g_targets.end() || !it->second || !it->second->texture ||
      it->second->samples != plume::RenderSampleCount::COUNT_1)
    return nullptr;
  return it->second.get();
}

// blur-nx (2026-11-10, port 76; its key 215; see surfaces.h): the operation being replayed (render thread).
const StackHeaderSet *g_replay_stack_headers = nullptr;

void CaptureStackHeaders(const u8 *device, StackHeaderSet &out) {
  out.count = 0;
  if (!device || !banjo::renut_gpu::StackSurfaceHeadersOn())
    return;
  const auto be = [device](u32 off) {
    return u32(device[off]) << 24 | u32(device[off + 1]) << 16 | u32(device[off + 2]) << 8 | u32(device[off + 3]);
  };
  const u32 vas[5] = {be(d3d::kDeviceOffsetRenderTargets), be(d3d::kDeviceOffsetRenderTargets + 4),
                      be(d3d::kDeviceOffsetRenderTargets + 8), be(d3d::kDeviceOffsetRenderTargets + 12),
                      be(d3d::kDeviceOffsetDepthStencil)};
  for (const u32 va : vas) {
    if (!va || !CapturedHeaderVa(va) || out.count >= 5)  // port 174: every bound surface with the key on
      continue;
    // Port 174 (rehatched-nx): with the key on, the header as it was when the surface was bound (NoteSurfaceHeader), as
    // the console's GPU has it.
    if (AllSurfaceHeadersOn()) {
      std::lock_guard lock(g_stack_headers_mutex);
      if (const auto it = g_stack_headers.find(va); it != g_stack_headers.end()) {
        out.va[out.count] = va;
        out.desc[out.count] = it->second;
        ++out.count;
        continue;
      }
    }
    if (const auto *desc = GuestPtr<const d3d::SurfaceDesc>(va + sizeof(d3d::Resource))) {
      out.va[out.count] = va;
      out.desc[out.count] = *desc;
      ++out.count;
    }
  }
}

void SetReplayStackHeaders(const StackHeaderSet *set) { g_replay_stack_headers = set; }

void NoteSurfaceHeader(u32 surface_va) {
  if (!CapturedHeaderVa(surface_va) || !banjo::renut_gpu::StackSurfaceHeadersOn())  // port 174
    return;
  const auto *desc = GuestPtr<const d3d::SurfaceDesc>(surface_va + sizeof(d3d::Resource));
  if (!desc)
    return;
  std::lock_guard lock(g_stack_headers_mutex);
  g_stack_headers[surface_va] = *desc;
}

HostSurface *GetHostSurfaceLocked(u32 surface_va) {
  if (!surface_va)
    return nullptr;
  // Targets made before the first Swap are of the configured scale too.
  if (!g_resolution_latched)
    LatchResolutionScaleLocked();
  const auto *desc = GuestPtr<const d3d::SurfaceDesc>(surface_va + sizeof(d3d::Resource));
  if (!desc)
    return nullptr;
  d3d::SurfaceDesc stack_copy;  // blur-nx (2026-11-10, port 76; its key 215: blur_renut_stack_surface_headers)
  if (CapturedHeaderVa(surface_va) && banjo::renut_gpu::StackSurfaceHeadersOn()) {  // port 174
    bool found = false;
    if (const StackHeaderSet *set = g_replay_stack_headers) {  // the replayed operation's own
      for (u32 i = 0; i < set->count; ++i) {
        if (set->va[i] == surface_va) {
          stack_copy = set->desc[i];
          desc = &stack_copy;
          found = true;
          break;
        }
      }
    }
    // port 174: the last-bound fallback stays for the stack's (a heap header the operation did not capture is read live)
    if (!found && StackVa(surface_va)) {  // a path that captured none: as last bound (counted: none is expected)
      std::lock_guard lock(g_stack_headers_mutex);
      if (const auto it = g_stack_headers.find(surface_va); it != g_stack_headers.end()) {
        stack_copy = it->second;
        desc = &stack_copy;
        static uint64_t fallbacks = 0;
        if (const uint64_t n = ++fallbacks; n <= 4 || (n & (n - 1)) == 0)
          RGPU_WARN("[renut] stack surface {:08X} read with its last bound header: the operation captured none ({} so "
                    "far; blur_renut_stack_surface_headers)", surface_va, n);
      }
    }
  }

  const bool memo = banjo::renut_gpu::CpuMemosOn();
  SurfaceMemo &m = g_surface_memo[(surface_va * 0x9E3779B1u) >> 26];
  if (memo && m.epoch == g_surface_epoch && m.va == surface_va && m.size == desc->Size &&
      m.info == desc->SurfaceInfo && m.format == desc->Format && m.color_info == desc->ColorInfo) {
    // the same as the lookup's: the surface bound now
    m.surface->size_word = desc->Size;
    m.surface->info_word = desc->SurfaceInfo;
    m.surface->format_word = desc->Format;
    m.surface->color_info_word = desc->ColorInfo;  // halo3-nx
    m.surface->sasrt_used_frame = g_sasrt_frames;  // sasrt-nx (2026-11-09, sasrt_retire_idle_targets_frames; port 58)
    // banjo-nx (2026-10-21, internal resolution): the composite's target after the composite: its UI target.
    if (g_ui.redirect && m.surface == g_ui.scene)
      return g_ui.surface.get();
    if (g_ui_depth.redirect && m.surface == g_ui_depth.scene)
      return g_ui_depth.surface.get();
    return m.surface;
  }
  HostSurface *result = GetHostSurfaceSlow(surface_va, desc);
  if (result)
    result->sasrt_used_frame = g_sasrt_frames;  // sasrt-nx (2026-11-09, sasrt_retire_idle_targets_frames; port 58)
  if (memo && result) {
    m.va = surface_va;
    m.size = desc->Size;
    m.info = desc->SurfaceInfo;
    m.format = desc->Format;
    m.color_info = desc->ColorInfo;  // port 161
    m.epoch = g_surface_epoch;
    m.surface = result;
  }
  if (g_ui.redirect && result && result == g_ui.scene)
    return g_ui.surface.get();  // banjo-nx (2026-10-21, internal resolution)
  if (g_ui_depth.redirect && result && result == g_ui_depth.scene)
    return g_ui_depth.surface.get();
  return result;
}

// banjo-nx (2026-10-21, internal resolution): see surfaces.h.
RenderScale UiScale() { return g_ui_scale; }

bool UiRedirectedLocked() { return g_ui.redirect; }

void NoteCompositeLocked() { g_composite_seen = true; }

void RedirectToUiSurfaceLocked() {
  if (g_ui.scene && g_ui.surface && g_ui.surface->texture && !g_ui.redirect) {
    g_ui.redirect = true;
    ++g_surface_epoch;
  }
}

void RedirectDepthToUiLocked() {
  if (g_ui_depth.scene && g_ui_depth.surface && g_ui_depth.surface->texture && !g_ui_depth.redirect) {
    g_ui_depth.redirect = true;
    ++g_surface_epoch;
  }
}

uint32_t InternalHeightsWord() { return g_internal_heights; }

bool ConsoleDocked() {
#if defined(__SWITCH__)
  return rex::ui::switch_saltynx::BaseMode(appletGetOperationMode() == AppletOperationMode_Console);
#else
  return false;
#endif
}

namespace {

// The UI target 'u' for 'scene' (colour or depth): kept while it fits, made again otherwise.
HostSurface *MakeUiTarget(UiTarget &u, HostSurface *scene) {
  if (!scene || !scene->texture)
    return nullptr;
  HostSurface *ui = u.surface.get();
  if (ui && ui->texture && u.scene == scene && u.scene_generation == scene->generation &&
      ui->scale == g_ui_scale && ui->width == scene->width && ui->height == scene->height &&
      ui->format == scene->format && ui->samples == scene->samples) {
    ui->size_word = scene->size_word;
    ui->info_word = scene->info_word;
    ui->format_word = scene->format_word;
    return ui;
  }
  if (!ui) {
    u.surface = std::make_unique<HostSurface>();
    ui = u.surface.get();
  } else if (ui->texture) {
    RetireTarget(*ui);
  }
  ui->guest_va = scene->guest_va;
  ui->size_word = scene->size_word;
  ui->info_word = scene->info_word;
  ui->format_word = scene->format_word;
  ui->width = scene->width;
  ui->height = scene->height;
  ui->scale = g_ui_scale;
  ui->depth = scene->depth;
  ui->format = scene->format;
  ui->samples = scene->samples;
  ui->writes = 0;
  std::fill(std::begin(ui->profile_labels), std::end(ui->profile_labels), UINT32_MAX);
  const uint32_t host_width = ui->scale.Size(ui->width), host_height = ui->scale.Size(ui->height);
  plume::RenderDevice *device = Video::Device();
  const plume::RenderMultisampling multisampling(ui->samples);
  ui->texture = device->createTexture(
      ui->depth ? plume::RenderTextureDesc::DepthTarget(host_width, host_height, ui->format, multisampling)
                : plume::RenderTextureDesc::ColorTarget(host_width, host_height, ui->format, multisampling));
  ui->layout = plume::RenderTextureLayout::UNKNOWN;
  ui->generation = g_next_generation++;
  ++g_surface_epoch;
  if (!TextureCreated(ui->texture.get())) {
    ui->texture.reset();
    u.scene = nullptr;
    if (!ReportDeviceLost(device))
      RGPU_ERROR("UI {} target for {:08X}: {}x{} creation failed", ui->depth ? "depth" : "colour", scene->guest_va,
                 host_width, host_height);
    return nullptr;
  }
  ui->texture->setName(ui->depth ? "renut UI depth surface" : "renut UI surface");
  u.scene = scene;
  u.scene_generation = scene->generation;
  RGPU_INFO("UI {} target for {:08X}: {}x{} (the 3D image {}x{})", ui->depth ? "depth" : "colour", scene->guest_va,
            host_width, host_height, scene->scale.Size(scene->width), scene->scale.Size(scene->height));
  return ui;
}

}  // namespace

HostSurface *UiSurfaceLocked(HostSurface *scene) {
  return scene && !scene->depth ? MakeUiTarget(g_ui, scene) : nullptr;
}

HostSurface *UiDepthSurfaceLocked(HostSurface *scene_depth) {
  return scene_depth && scene_depth->depth ? MakeUiTarget(g_ui_depth, scene_depth) : nullptr;
}

HostSurface *GetHostSurfaceSlow(u32 surface_va, const d3d::SurfaceDesc *desc) {
  // blur-nx (2026-11-10, port 76; its key 217: blur_renut_surface_size_guard): no Xbox 360 target is wider or taller than
  // 8192; a header that reads so is not one (a temporary on the stack, read after the stack was reused).
  if (banjo::renut_gpu::SurfaceSizeGuardOn() &&
      (d3d::SurfaceWidth(desc->Size) > 8192 || d3d::SurfaceHeight(desc->Size) > 8192)) {
    static uint32_t logged = 0;
    if (logged++ < 8)
      RGPU_WARN("[renut] surface {:08X}: its header reads {}x{}: no target made (blur_renut_surface_size_guard)",
                surface_va, d3d::SurfaceWidth(desc->Size), d3d::SurfaceHeight(desc->Size));
    return nullptr;
  }
  const bool depth = d3d::IsDepthFormat(desc->Format);
  plume::RenderFormat format = depth ? DepthTargetFormat(desc->Format) : SurfaceColorFormat(*desc);
  // The render resolution, or the finer one graphics_options gives it.
  const RenderScale scale = SurfaceRenderScale(*desc, g_resolution_scale);
  if (depth && banjo::renut_gpu::Depth24On()) {
    // banjo-nx (2026-10-19, banjo_deko3d_depth24): one view, one format (g_depth_view_formats).
    uint8_t &seen = g_depth_view_formats[TargetKey(*desc, true, plume::RenderFormat::UNKNOWN, scale)];
    const uint8_t bit = (desc->Format & 0x3F) == 23 ? 2 : 1;
    if ((seen | bit) == 3 && seen != 3) {
      static uint32_t logged = 0;
      if (logged++ < 8)
        RGPU_WARN("Depth view of surface {:08X} bound as both D24S8 and D24FS8: 32-bit float from now on "
                  "(banjo_deko3d_depth24)", surface_va);
    }
    seen |= bit;
    if (seen & 2)
      format = DepthTargetFormat();
  }
  const uint64_t key = TargetKey(*desc, depth, format, scale);
  auto [bound, first_use] = g_surface_keys.try_emplace(surface_va, key);
  if (first_use) {
    UseTarget(key);  // port 145: a kept target taken again
    ++g_surface_epoch;
  } else if (bound->second != key) {
    ++g_surface_epoch;
    // The header was rewritten (or the VA reused without Destroy reaching
    // us): the surface is a view of other EDRAM now.
    UnuseTarget(bound->second);
    bound->second = key;
    UseTarget(key);  // port 145
  }

  auto &slot = g_targets[key];
  // The view's target covers every surface over it, and every tile while
  // tiling (SetTilingExtentLocked).
  uint32_t min_width = std::max(g_tiling_width, d3d::SurfaceWidth(desc->Size));
  uint32_t min_height = std::max(g_tiling_height, d3d::SurfaceHeight(desc->Size));
  if (slot && slot->texture) {
    // The surface bound now: its guest size (Targets::guest_width) and D3D
    // format (D24S8 or D24FS8 for a depth resolve).
    slot->size_word = desc->Size;
    slot->info_word = desc->SurfaceInfo;
    slot->format_word = desc->Format;
    slot->color_info_word = desc->ColorInfo;  // halo3-nx
    if (slot->width >= min_width && slot->height >= min_height && slot->scale == scale)
      return slot.get();
    // A larger surface over the view, or a larger tiling: grow it, keeping
    // the size the others gave it (what it held is lost). (Another scale is
    // another target: TargetKey.)
    min_width = std::max(min_width, slot->width);
    min_height = std::max(min_height, slot->height);
    RetireTarget(*slot);
  } else if (!slot) {
    slot = std::make_unique<HostSurface>();
    slot->guest_va = surface_va;
  }
  ++g_surface_epoch;
  ++g_sasrt_table_epoch;  // sasrt-nx (2026-11-09, sasrt_rt_restore_candidates; port 54): a target built
  if (!BuildTarget(*slot, *desc, min_width, min_height, scale))
    return nullptr;
  // No defined content yet; D3D12 wants a new target discarded (or cleared)
  // before anything else uses it, and a resolve may read it before any draw.
  if (plume::RenderCommandList *cmd = Video::CommandListLocked()) {
    TransitionSurfaceLocked(slot.get(), slot->depth ? plume::RenderTextureLayout::DEPTH_WRITE
                                                    : plume::RenderTextureLayout::COLOR_WRITE);
    cmd->discardTexture(slot->texture.get());
  }
  return slot.get();
}

// rr6-nx (2026-11-02, rr6_depth_covers_colour): RB_SURFACE_INFO's pitch is the same for every target and a depth surface's
// own size is only D3D's bookkeeping, so the console draws a 1440x800 colour target over a depth surface made 1280x720 (Ridge
// Racer 6's race). A host framebuffer can't: Direct3D 12 cuts the draws to the depth's size (black bars on the right and at the
// bottom) and in Vulkan it is an invalid framebuffer (device lost). The view's target grows once, as for a larger surface over
// it in GetHostSurfaceSlow, and keeps that size. What it held is lost: the game clears it next.
HostSurface *GrowDepthToColourLocked(u32 surface_va, HostSurface *depth, uint32_t width, uint32_t height) {
  const auto *desc = GuestPtr<const d3d::SurfaceDesc>(surface_va + sizeof(d3d::Resource));
  if (!desc || FindHostSurfaceLocked(surface_va) != depth)
    return depth;
  const uint32_t min_width = std::max(width, depth->width), min_height = std::max(height, depth->height);
  RGPU_INFO("Depth target {:08X}: {}x{} grown to {}x{}, the colour target over it (rr6_depth_covers_colour)", surface_va,
            depth->width, depth->height, min_width, min_height);
  RetireTarget(*depth);
  ++g_surface_epoch;
  ++g_sasrt_table_epoch;  // sasrt-nx (2026-11-09, sasrt_rt_restore_candidates; port 54)
  if (!BuildTarget(*depth, *desc, min_width, min_height, depth->scale))
    return nullptr;
  if (plume::RenderCommandList *cmd = Video::CommandListLocked()) {
    TransitionSurfaceLocked(depth, plume::RenderTextureLayout::DEPTH_WRITE);
    cmd->discardTexture(depth->texture.get());
  }
  return depth;
}

uint64_t g_surface_write_seq = 0;  // sasrt-nx (2026-10-20)
uint64_t g_blur_color_write_last = 0;  // blur-nx (2026-11-11, port 76; its key 220: blur_renut_edram_inherit)

HostSurface *DepthRestoreSourceLocked(const HostSurface *depth) {
  if (!depth || !depth->depth || depth->samples != plume::RenderSampleCount::COUNT_1)
    return nullptr;
  // sasrt-nx (2026-11-09, sasrt_rt_restore_candidates; the render-thread round; port 54): the depth target's own candidates
  // (the two scans below, found again when the table's epoch moved), then the same test of their write sequences. The
  // fast path asks for nearly every draw with depth bound (~430 calls a frame in Sonic Transformed's race).
  if (sasrt::gpu_round::On(sasrt::gpu_round::kRtRestoreCandidates)) {
    HostSurface *d = const_cast<HostSurface *>(depth);
    if (d->sasrt_candidates_epoch != g_sasrt_table_epoch) [[unlikely]] {
      d->sasrt_restore_candidates.clear();
      d->sasrt_candidates_epoch = g_sasrt_table_epoch;
      uint64_t own_key = 0;
      bool own_found = false;
      for (const auto &[key, target] : g_targets)
        if (target.get() == depth) {
          own_key = key;
          own_found = true;
          break;
        }
      constexpr uint64_t kBaseAndPitchMask = 0x3FFFFFFull;  // TargetKey: the EDRAM base in bits 0-11, the pitch in 12-25
      if (own_found)
        for (const auto &[key, target] : g_targets) {
          HostSurface *c = target.get();
          // blur-nx (2026-11-13, MSAA lowered): and the console's sample count (TargetKey bits 26-27): msaa_level makes a
          // 4x depth view single-sampled here, which must not take a true 1x colour view's pixels (another layout).
          if (c && !c->depth && (key & kBaseAndPitchMask) == (own_key & kBaseAndPitchMask) &&
              ((key >> 26) & 3) == ((own_key >> 26) & 3) &&
              c->samples == plume::RenderSampleCount::COUNT_1 && c->scale == depth->scale &&
              (c->format == plume::RenderFormat::R8G8B8A8_UNORM || c->format == plume::RenderFormat::B8G8R8A8_UNORM))
            d->sasrt_restore_candidates.push_back(c);
        }
      sasrt::gpu_round::Count(sasrt::gpu_round::kRtRestoreCandidates, 1, 1);
    }
    HostSurface *best = nullptr;
    for (HostSurface *c : d->sasrt_restore_candidates) {
      if (!c->texture || c->write_seq <= depth->write_seq || c->write_seq <= depth->restored_from_seq)
        continue;
      if (!best || c->write_seq > best->write_seq)
        best = c;
    }
    sasrt::gpu_round::Count(sasrt::gpu_round::kRtRestoreCandidates);
    return best;
  }
  uint64_t own = 0;
  bool found = false;
  for (const auto &[key, target] : g_targets) {
    if (target.get() == depth) {
      own = key;
      found = true;
      break;
    }
  }
  if (!found)
    return nullptr;
  // TargetKey: the EDRAM base in bits 0-11, the pitch in 12-25.
  constexpr uint64_t kBaseAndPitch = 0x3FFFFFFull;
  HostSurface *best = nullptr;
  for (const auto &[key, target] : g_targets) {
    HostSurface *c = target.get();
    if (!c || c->depth || !c->texture || (key & kBaseAndPitch) != (own & kBaseAndPitch) ||
        ((key >> 26) & 3) != ((own >> 26) & 3) ||  // blur-nx (2026-11-13): the console's sample count too (above)
        c->samples != plume::RenderSampleCount::COUNT_1 || c->scale != depth->scale ||
        (c->format != plume::RenderFormat::R8G8B8A8_UNORM && c->format != plume::RenderFormat::B8G8R8A8_UNORM))
      continue;
    if (c->write_seq <= depth->write_seq || c->write_seq <= depth->restored_from_seq)
      continue;
    if (!best || c->write_seq > best->write_seq)
      best = c;
  }
  return best;
}

HostSurface *SurfaceByGenerationLocked(uint64_t generation) {
  for (const auto &[key, surface] : g_targets)
    if (surface && surface->texture && surface->generation == generation)
      return surface.get();
  for (const UiTarget *u : {&g_ui, &g_ui_depth})  // banjo-nx (2026-10-21): the UI targets are no view's
    if (u->surface && u->surface->texture && u->surface->generation == generation)
      return u->surface.get();
  return nullptr;
}

void SurfaceMemoryLocked(size_t &targets, uint64_t &bytes) {
  targets = 0;
  bytes = 0;
  for (const auto &[key, surface] : g_targets) {
    if (!surface || !surface->texture)
      continue;
    ++targets;
    const uint64_t texel = plume::RenderFormatSize(surface->format);
    bytes += uint64_t(surface->scale.Size(surface->width)) * surface->scale.Size(surface->height) *
             texel * surface->samples;
  }
  for (const UiTarget *u : {&g_ui, &g_ui_depth}) {  // banjo-nx (2026-10-21)
    if (const HostSurface *ui = u->surface.get(); ui && ui->texture) {
      ++targets;
      bytes += uint64_t(ui->scale.Size(ui->width)) * ui->scale.Size(ui->height) *
               plume::RenderFormatSize(ui->format) * ui->samples;
    }
  }
}

void ReleaseHostSurfaceLocked(u32 surface_va) {
  auto it = g_surface_keys.find(surface_va);
  if (it == g_surface_keys.end())
    return;
  const uint64_t key = it->second;
  g_surface_keys.erase(it);
  ++g_surface_epoch;
  UnuseTarget(key);
}

uint32_t SurfaceSampleSlotLocked(HostSurface *surface) {
  if (!surface || !surface->texture)
    return kInvalidDescriptor;
  if (surface->sample_slot != UINT32_MAX)
    return surface->sample_slot;
  // The depth format views the depth plane.
  surface->sample_view = surface->texture->createTextureView(
      plume::RenderTextureViewDesc::Texture2D(surface->format));
  if (!surface->sample_view)
    return kInvalidDescriptor;
  const uint32_t slot = Video::AllocateTextureSlotLocked();
  if (slot == kInvalidDescriptor)
    return kInvalidDescriptor;
  Video::SetTextureSlotLocked(slot, surface->texture.get(), surface->sample_view.get());
  surface->sample_slot = slot;
  return slot;
}

// banjo-nx (2026-10-18): the bindless slot of a depth/stencil target's stencil view (read as uint by the depth
// resolve), or kInvalidDescriptor (no stencil, D3D12, or no view). Tried once a surface.
uint32_t SurfaceStencilSlotLocked(HostSurface *surface) {
  if (!surface || !surface->texture || !UsesVulkanBindings() || !plume::RenderFormatIsStencil(surface->format))
    return kInvalidDescriptor;
  if (surface->stencil_slot != UINT32_MAX)
    return surface->stencil_slot;
  surface->stencil_slot = kInvalidDescriptor;
  surface->stencil_view = surface->texture->createTextureView(
      plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::S8_UINT));
  if (!surface->stencil_view)
    return kInvalidDescriptor;
  const uint32_t slot = Video::AllocateTextureSlotLocked();
  if (slot == kInvalidDescriptor)
    return kInvalidDescriptor;
  Video::SetTextureSlotLocked(slot, surface->texture.get(), surface->stencil_view.get());
  surface->stencil_slot = slot;
  return slot;
}

void TransitionSurfaceLocked(HostSurface *surface, plume::RenderTextureLayout layout) {
  if (!surface || !surface->texture || surface->layout == layout)
    return;
  ++g_layout_epoch;  // banjo-nx (2026-10-26, render/textures.h)
  NoteGpuWriteLayoutLocked(layout);  // port 161 (banjo_deko3d_vertex_texture_barrier)
  // banjo-nx (2026-10-19, banjo_deko3d_barrier_lean): from one read layout to another: no barrier (textures.cpp).
  if (IsDeko3D() && banjo::renut_gpu::Deko3DBarrierLeanOn() && ReadOnlyLayout(surface->layout) &&
      ReadOnlyLayout(layout)) {
    surface->layout = layout;
    return;
  }
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!cmd)
    return;
  // sasrt-nx (2026-11-04, sasrt_deko3d_copy_wait): see TransitionTextureLocked (textures.cpp).
  const auto copy_layout = [](plume::RenderTextureLayout l) {
    return l == plume::RenderTextureLayout::COPY_SOURCE || l == plume::RenderTextureLayout::COPY_DEST ||
           l == plume::RenderTextureLayout::RESOLVE_SOURCE || l == plume::RenderTextureLayout::RESOLVE_DEST;
  };
  const bool from_copy = IsDeko3D() && banjo::renut_gpu::Deko3DCopyWaitOn() && copy_layout(surface->layout);
  cmd->barriers(copy_layout(layout) || from_copy ? plume::RenderBarrierStage::COPY
                                                 : plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(surface->texture.get(), layout));
  surface->layout = layout;
}

void SwapDepthImageLocked(HostSurface &surface, DepthImage &image) {
  ++g_surface_epoch;
  std::swap(surface.texture, image.texture);
  std::swap(surface.layout, image.layout);
  std::swap(surface.generation, image.generation);
  std::swap(surface.sample_view, image.sample_view);
  std::swap(surface.sample_slot, image.sample_slot);
  std::swap(surface.stencil_view, image.stencil_view);
  std::swap(surface.stencil_slot, image.stencil_slot);
  std::swap(surface.handover_views, image.views);
  image.host_width = surface.scale.Size(surface.width);
  image.host_height = surface.scale.Size(surface.height);
  image.format = surface.format;
  image.samples = surface.samples;
  image.format_word = surface.format_word;
}

bool MakeDepthImageLocked(const HostSurface &surface, DepthImage &image) {
  plume::RenderDevice *device = Video::Device();
  const uint32_t w = surface.scale.Size(surface.width), h = surface.scale.Size(surface.height);
  // kameo-nx (2026-11-03, banjo_deko3d_color_handover): a colour target's image is made as GetHostSurfaceLocked makes it.
  image.texture = device->createTexture(
      surface.depth
          ? plume::RenderTextureDesc::DepthTarget(w, h, surface.format, plume::RenderMultisampling(surface.samples))
          : plume::RenderTextureDesc::ColorTarget(w, h, surface.format, plume::RenderMultisampling(surface.samples)));
  if (!TextureCreated(image.texture.get())) {
    image.texture.reset();
    ReportDeviceLost(device);
    return false;
  }
  image.texture->setName("renut depth surface (hand-over)");
  image.layout = plume::RenderTextureLayout::UNKNOWN;
  image.generation = g_next_generation++;
  image.host_width = w;
  image.host_height = h;
  image.format = surface.format;
  image.samples = surface.samples;
  image.format_word = surface.format_word;
  return true;
}

void RetireDepthImageLocked(DepthImage &image) {
  // The framebuffers a surface made over this image (cached by its generation).
  for (size_t i = 0; i < g_framebuffers.size();) {
    bool over_image = g_framebuffers[i].key.depth && g_framebuffers[i].key.depth_generation == image.generation;
    for (uint32_t c = 0; c < d3d::kMaxRenderTargets; ++c)  // kameo-nx (2026-11-03): a colour target's image
      over_image = over_image || (g_framebuffers[i].key.colors[c] && g_framebuffers[i].key.color_generations[c] == image.generation);
    if (over_image) {
      Video::RetireLocked(std::move(g_framebuffers[i].framebuffer));
      g_framebuffers[i] = std::move(g_framebuffers.back());
      g_framebuffers.pop_back();
      ++g_framebuffer_epoch;
    } else {
      ++i;
    }
  }
  FreeDepthViews(image.views);
  if (image.sample_slot != UINT32_MAX && image.sample_slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(image.sample_slot);
  image.sample_slot = UINT32_MAX;
  Video::RetireLocked(std::move(image.sample_view));
  if (image.stencil_slot != UINT32_MAX && image.stencil_slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(image.stencil_slot);
  image.stencil_slot = UINT32_MAX;
  Video::RetireLocked(std::move(image.stencil_view));
  Video::RetireLocked(std::move(image.texture));
}

void TransitionDepthImageLocked(DepthImage &image, plume::RenderTextureLayout layout) {
  if (!image.texture || image.layout == layout)
    return;
  ++g_layout_epoch;  // banjo-nx (2026-10-26, render/textures.h)
  NoteGpuWriteLayoutLocked(layout);  // port 161 (banjo_deko3d_vertex_texture_barrier)
  if (IsDeko3D() && banjo::renut_gpu::Deko3DBarrierLeanOn() && ReadOnlyLayout(image.layout) && ReadOnlyLayout(layout)) {
    image.layout = layout;
    return;
  }
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!cmd)
    return;
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(image.texture.get(), layout));
  image.layout = layout;
}

uint32_t DepthImageViewSlotLocked(plume::RenderTexture *texture, plume::RenderFormat format,
                                  std::vector<DepthImage::View> &views, const plume::RenderComponentMapping &mapping) {
  if (!texture)
    return kInvalidDescriptor;
  for (const DepthImage::View &v : views) {
    if (v.mapping.r == mapping.r && v.mapping.g == mapping.g && v.mapping.b == mapping.b && v.mapping.a == mapping.a)
      return v.slot;
  }
  // The depth format views the depth plane (deko3d samples it in the red channel), with the destination's mapping.
  plume::RenderTextureViewDesc desc = plume::RenderTextureViewDesc::Texture2D(format);
  desc.componentMapping = mapping;
  DepthImage::View v;
  v.mapping = mapping;
  v.view = texture->createTextureView(desc);
  if (!v.view)
    return kInvalidDescriptor;
  v.slot = Video::AllocateTextureSlotLocked();
  if (v.slot == kInvalidDescriptor) {
    Video::RetireLocked(std::move(v.view));
    return kInvalidDescriptor;
  }
  Video::SetTextureSlotLocked(v.slot, texture, v.view.get());
  views.push_back(std::move(v));
  return views.back().slot;
}

plume::RenderFramebuffer *GetFramebufferLocked(HostSurface *const *colors, uint32_t color_count,
                                               HostSurface *depth) {
  FramebufferKey key;
  for (uint32_t i = 0; i < color_count && i < d3d::kMaxRenderTargets; ++i) {
    key.colors[i] = colors[i];
    key.color_generations[i] = colors[i] ? colors[i]->generation : 0;
  }
  key.depth = depth;
  key.depth_generation = depth ? depth->generation : 0;
  // banjo-nx (2026-10-19, render-thread proposal 8): the recent ones first, the found one moved to the front.
  for (uint32_t i = 0; i < 4; ++i) {
    RecentFramebuffer &r = g_recent_framebuffers[i];
    if (r.epoch == g_framebuffer_epoch && r.framebuffer && r.key == key) {
      plume::RenderFramebuffer *found = r.framebuffer;
      if (i) {
        const RecentFramebuffer hit = r;
        for (uint32_t j = i; j > 0; --j)
          g_recent_framebuffers[j] = g_recent_framebuffers[j - 1];
        g_recent_framebuffers[0] = hit;
      }
      return found;
    }
  }
  auto remember = [&key](plume::RenderFramebuffer *framebuffer) {
    for (uint32_t j = 3; j > 0; --j)
      g_recent_framebuffers[j] = g_recent_framebuffers[j - 1];
    g_recent_framebuffers[0] = {key, framebuffer, g_framebuffer_epoch};
    return framebuffer;
  };
  for (auto &cached : g_framebuffers) {
    if (cached.key == key)
      return remember(cached.framebuffer.get());
  }

  const plume::RenderTexture *attachments[d3d::kMaxRenderTargets] = {};
  uint32_t count = 0;
  for (uint32_t i = 0; i < color_count && i < d3d::kMaxRenderTargets; ++i) {
    if (!colors[i] || !colors[i]->texture)
      break;  // host framebuffers need contiguous color attachments
    attachments[count++] = colors[i]->texture.get();
  }
  plume::RenderFramebufferDesc desc(attachments, count,
                                    depth && depth->texture ? depth->texture.get() : nullptr);
  auto framebuffer = Video::Device()->createFramebuffer(desc);
  if (!framebuffer) {
    RGPU_ERROR("Framebuffer creation failed ({} color targets, depth {})", count, depth != nullptr);
    return nullptr;
  }
  g_framebuffers.push_back({key, std::move(framebuffer)});
  return remember(g_framebuffers.back().framebuffer.get());
}

} // namespace renut::gpu
