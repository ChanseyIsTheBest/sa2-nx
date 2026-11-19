/**
 * @file    gpu/render/graphics_options.cpp
 */
#include "renut_engine/gpu/render/graphics_options.h"

#include <cstdlib>
#include <string>

#include <rex/cvar.h>
#include <rex/types.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/guest/widescreen.h"
#include "renut_engine/gpu/render/format.h"

REXCVAR_DEFINE_STRING(renut_shadow_resolution, "Resolution", "Nuts&Bolts/Graphics",
                      "Shadow map resolution, as a multiple of the console's: Resolution follows "
                      "renut_resolution; 1x is the console's; 2x, 3x and 4x are sharper.")
    .allowed({"Resolution", "1x", "2x", "3x", "4x"});

REXCVAR_DEFINE_STRING(renut_ambient_occlusion, "Half", "Nuts&Bolts/Graphics",
                      "Ambient occlusion (SSAO) resolution: Half the screen's, as on the console, "
                      "or Full.")
    .allowed({"Half", "Full"});

REXCVAR_DEFINE_BOOL(banjo_renut_dynamic_resolution, false, "Nuts&Bolts/Graphics",
                    "Dynamic resolution (2026-10-28, the user's request; not pixel-identical while it acts): the 3D "
                    "image between banjo_renut_dynamic_handheld_low and _high in handheld mode, _docked_low and _high "
                    "docked (in place of banjo_renut_render_height_handheld / _docked): the low height while the GPU's "
                    "time a frame is above banjo_renut_dynamic_gpu_ms, the high one when it would fit again (at least "
                    "5 s between changes). One end of each range is 720 lines (the 1x scale) on the PC's back ends; "
                    "on deko3d any range, started at its step nearest below 720 (sasrt-nx, port 52)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_renut_dynamic_handheld_low, 540, "Nuts&Bolts/Graphics",
                     "banjo_renut_dynamic_resolution: the handheld range's low height (a multiple of 9)")
    .range(360, 1080)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_renut_dynamic_handheld_high, 720, "Nuts&Bolts/Graphics",
                     "banjo_renut_dynamic_resolution: the handheld range's high height")
    .range(360, 1080)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_renut_dynamic_docked_low, 720, "Nuts&Bolts/Graphics",
                     "banjo_renut_dynamic_resolution: the docked range's low height")
    .range(360, 1080)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_renut_dynamic_docked_high, 1080, "Nuts&Bolts/Graphics",
                     "banjo_renut_dynamic_resolution: the docked range's high height (a multiple of 9; 1080 is the "
                     "docked display's own size, 900 before night_gpu5)")
    .range(360, 1080)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_renut_dynamic_steps, 4, "Nuts&Bolts/Graphics",
                     "banjo_renut_dynamic_resolution (night_gpu5, the user's request): heights between each range's low "
                     "and high, evenly spaced (multiples of 36: 576 612 648 684 handheld, 792 864 936 1008 docked); the "
                     "GPU's time picks the highest that fits. 0: only the two ends")
    .range(0, 8)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// sasrt-nx (2026-11-09, the user: "more steps, especially in docked"): each mode's grid of heights (a multiple of 9:
// whole host pixels across the 1280-wide screen). 36 (banjo's) is whole pixels at 320x180 too; 18 at 640x360, and a
// quarter-size target's edge then rounds to the nearest pixel (RenderScale::Coord), half a pixel off at most.
// vivapinata-nx (2026-11-12, the owner: 6 steps, then "Yes, port the grid"): 540-720 has room for only 4 heights on 36
// lines; Viva's built-in settings take 18 handheld (app/vivapinata_switch_defaults.toml).
REXCVAR_DEFINE_INT32(sasrt_dynamic_lines_handheld, 36, "Nuts&Bolts/Graphics",
                     "banjo_renut_dynamic_resolution: the handheld steps are multiples of this many lines (9 to 72, a "
                     "multiple of 9; 36 banjo's; from sasrt-nx)")
    .range(9, 72)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sasrt_dynamic_lines_docked, 36, "Nuts&Bolts/Graphics",
                     "banjo_renut_dynamic_resolution: the docked steps are multiples of this many lines (9 to 72, a "
                     "multiple of 9; 36 banjo's; from sasrt-nx)")
    .range(9, 72)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(banjo_renut_dynamic_up_s, 1.0, "Nuts&Bolts/Graphics",
                      "banjo_renut_dynamic_resolution: seconds in a row the high height must be predicted to fit (and "
                      "since the last change) before it is taken again (0.1 to 600; 1 = quick, the user's choice)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-28): 34.5 (the user's choice; a decimal key since then), 40 before.
REXCVAR_DEFINE_DOUBLE(banjo_renut_dynamic_gpu_ms, 34.5, "Nuts&Bolts/Graphics",
                      "banjo_renut_dynamic_resolution: the GPU's time a frame (ms, a moving average) above which the low "
                      "height is taken (10 to 200)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_renut_dynamic_test_s, 0, "Nuts&Bolts/Graphics",
                     "banjo_renut_dynamic_resolution test: alternate the two heights every N seconds whatever the load "
                     "(0: by the GPU's time)")
    .range(0, 600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_renut_anisotropy_cap, 0, "Nuts&Bolts/Graphics",
                     "NOT pixel-identical (2026-10-28, GPU block, the user's call): the most anisotropic filtering any "
                     "texture gets: 0 the game's own (as on the Xbox 360), 1 none, 2/4/8 at most that ratio. The "
                     "heaviest shaders read 14-21 textures a pixel; a high ratio multiplies each read. Compare runs "
                     "with and without it (the renderer's GPU ms)")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(renut_anisotropic_filtering, "Game", "Nuts&Bolts/Graphics",
                      "Anisotropic texture filtering: the game's own, or at least 2x, 4x, 8x or "
                      "16x.")
    .allowed({"Game", "2x", "4x", "8x", "16x"});

REXCVAR_DEFINE_STRING(renut_post_process_precision, "High", "Nuts&Bolts/Graphics",
                      "Precision of the bloom, depth of field and SSAO buffers: Game keeps the "
                      "console's 8 bits (faint glow shows as blobs sliding over dark areas), High "
                      "uses 16 bits.")
    .allowed({"Game", "High"});

namespace renut::gpu {

namespace {

// D3DFORMAT low bits of D3DFMT_D24S8 (the shadow maps; the scene's depth is
// D24FS8).
constexpr u32 kFormatD24S8 = 22;
// The SSAO targets' EDRAM base (sub_8232E488).
constexpr u32 kAmbientOcclusionEdramBase = 1864;
// The game's screen height (widescreen.h).
constexpr u32 kGuestScreenHeight = 720;

bool IsShadowMap(const d3d::SurfaceDesc &desc, u32 width, u32 height) {
  if (width != height)
    return false;
  if ((u32(desc.Format) & 0x3F) == kFormatD24S8)
    return width == 512 || width == 768 || width == 1024;
  // The local lights' depth, converted for their atlas.
  return !d3d::IsDepthFormat(desc.Format) && width == 512 &&
         GpuColorFormat((u32(desc.ColorInfo) >> 16) & 0xF) == GpuColorFormat::Fmt16_16_Float;
}

bool IsAmbientOcclusion(const d3d::SurfaceDesc &desc, u32 width, u32 height) {
  return !d3d::IsDepthFormat(desc.Format) && width == 640 && height == 360 &&
         (u32(desc.ColorInfo) & 0xFFF) == kAmbientOcclusionEdramBase;
}

// A quarter of the screen or less: the post-processing buffers (depth of
// field and bloom, 320x180 down to 20x11 at 1280x720). Their blurs average a
// few taps 2 of their own texels apart, made for buffers box-downsampled from
// the screen; rendered at a higher scale, the buffers keep detail those taps
// step over, so edges come out as ghost copies rather than blurred.
bool IsPostProcessBuffer(const d3d::SurfaceDesc &desc, u32 width, u32 height) {
  return !d3d::IsDepthFormat(desc.Format) && width * 4 <= GuestScreenWidth() &&
         height * 4 <= kGuestScreenHeight;
}

// Half the screen or less, single-sampled, 8_8_8_8: the bloom's bright pass
// (640x360) and the post-processing buffers below it, and the SSAO targets.
bool IsLowPrecisionPostTarget(const d3d::SurfaceDesc &desc) {
  const u32 width = d3d::SurfaceWidth(desc.Size);
  const u32 height = d3d::SurfaceHeight(desc.Size);
  return !d3d::IsDepthFormat(desc.Format) && ((u32(desc.SurfaceInfo) >> 16) & 3) == 0 &&
         width * 2 <= GuestScreenWidth() && height * 2 <= kGuestScreenHeight &&
         GpuColorFormat((u32(desc.ColorInfo) >> 16) & 0xF) == GpuColorFormat::Fmt8_8_8_8;
}

// The options as LatchGraphicsOptionsLocked last read them: the draw path
// asks for every target and sampler of every draw, too often for the
// settings' strings.
struct Latched {
  bool shadow_follows_resolution = true;
  RenderScale shadow_scale;
  bool full_ambient_occlusion = false;
  uint32_t min_anisotropy = 1;
  bool high_precision_post = true;
};
Latched g_latched;

} // namespace

void LatchGraphicsOptionsLocked() {
  const std::string &shadows = REXCVAR_GET(renut_shadow_resolution);
  g_latched.shadow_follows_resolution = shadows == "Resolution" || shadows.empty();
  g_latched.shadow_scale = {};
  if (!g_latched.shadow_follows_resolution)
    RenderScale::FromRatio(uint32_t(std::atoi(shadows.c_str())), 1, g_latched.shadow_scale);
  g_latched.full_ambient_occlusion = REXCVAR_GET(renut_ambient_occlusion) == "Full";
  // banjo-nx (2026-10-18): deko3d draws every target at the console's scale (its shaders are made for 1x, pass scale1
  // of tools/deko3d_glsl_opt.py): shadow maps follow the resolution (1x) and the ambient occlusion is the game's.
  // banjo-nx (2026-10-21, internal resolution): the 3D image at 648p or 900p (and the menus at 1080p docked): the shadow
  // maps stay at the console's size (1x, the game's shadows; not 0.9x), the ambient occlusion the game's.
  if (IsDeko3D()) {
    g_latched.shadow_follows_resolution = false;
    g_latched.shadow_scale = {};
    g_latched.full_ambient_occlusion = false;
  }
  const int anisotropy = std::atoi(REXCVAR_GET(renut_anisotropic_filtering).c_str());
  g_latched.min_anisotropy = anisotropy >= 2 ? uint32_t(anisotropy) : 1;
  g_latched.high_precision_post = REXCVAR_GET(renut_post_process_precision) != "Game";
}

RenderScale SurfaceRenderScale(const d3d::SurfaceDesc &desc, RenderScale resolution) {
  if ((u32(desc.SurfaceInfo) >> 16) & 3)
    return resolution;  // multisampled: the scene's
  const u32 width = d3d::SurfaceWidth(desc.Size);
  const u32 height = d3d::SurfaceHeight(desc.Size);
  if (IsShadowMap(desc, width, height))
    return g_latched.shadow_follows_resolution ? resolution : g_latched.shadow_scale;
  if (g_latched.full_ambient_occlusion && IsAmbientOcclusion(desc, width, height))
    return resolution.Times(2);
  if (IsPostProcessBuffer(desc, width, height))
    return {};  // the console's
  // banjo-nx (2026-10-21, package 36z2): a depth target of a quarter of the screen or less goes with the colour targets
  // of that size (the post-processing buffers' rule, which takes colour only): a 128x128 one at 0.9x under its 128x128
  // colour target at 1x cut its draws to 115x115 (console log of 36z: ten draws a frame).
  if (IsDeko3D() && d3d::IsDepthFormat(desc.Format) && width * 4 <= GuestScreenWidth() &&
      height * 4 <= kGuestScreenHeight)
    return {};
  return resolution;
}

plume::RenderFormat SurfaceColorFormat(const d3d::SurfaceDesc &desc) {
  // 16-bit unorm keeps 8-bit's clamping to [0, 1], which the game's blends
  // and shaders were made for.
  if (g_latched.high_precision_post && IsLowPrecisionPostTarget(desc))
    return plume::RenderFormat::R16G16B16A16_UNORM;
  return ColorTargetFormat(GpuColorFormat((u32(desc.ColorInfo) >> 16) & 0xF));
}

bool DynamicResolutionOn() {
  static const bool v = REXCVAR_GET(banjo_renut_dynamic_resolution);
  return v;
}
int32_t DynamicHeight(bool docked, bool high) {
  static const int32_t v[4] = {int32_t(REXCVAR_GET(banjo_renut_dynamic_handheld_low)),
                               int32_t(REXCVAR_GET(banjo_renut_dynamic_handheld_high)),
                               int32_t(REXCVAR_GET(banjo_renut_dynamic_docked_low)),
                               int32_t(REXCVAR_GET(banjo_renut_dynamic_docked_high))};
  return v[(docked ? 2 : 0) + (high ? 1 : 0)];
}
double DynamicUpSeconds() {
  static const double set = double(REXCVAR_GET(banjo_renut_dynamic_up_s));
  static const double v = set < 0.1 ? 0.1 : set > 600.0 ? 600.0 : set;
  return v;
}
double DynamicGpuMs() {
  static const double set = double(REXCVAR_GET(banjo_renut_dynamic_gpu_ms));
  static const double v = set < 10.0 ? 10.0 : set > 200.0 ? 200.0 : set;
  return v;
}
int32_t DynamicTestSeconds() {
  static const int32_t v = int32_t(REXCVAR_GET(banjo_renut_dynamic_test_s));
  return v;
}
int32_t DynamicSteps() {
  static const int32_t v = int32_t(REXCVAR_GET(banjo_renut_dynamic_steps));
  return v < 0 ? 0 : v > 8 ? 8 : v;
}
int32_t DynamicStepLines(bool docked) {  // sasrt-nx (2026-11-09): sasrt_dynamic_lines_handheld / _docked
  static const int32_t h = int32_t(REXCVAR_GET(sasrt_dynamic_lines_handheld));
  static const int32_t d = int32_t(REXCVAR_GET(sasrt_dynamic_lines_docked));
  const int32_t v = (docked ? d : h) / 9 * 9;
  return v < 9 ? 9 : v > 72 ? 72 : v;
}

uint32_t AnisotropyCap() {
  static const int value = int(REXCVAR_GET(banjo_renut_anisotropy_cap));
  return value > 0 ? uint32_t(value) : 0u;
}

uint32_t MinAnisotropy() {
  return g_latched.min_anisotropy;
}

} // namespace renut::gpu
