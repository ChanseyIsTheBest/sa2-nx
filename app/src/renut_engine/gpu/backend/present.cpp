/**
 * @file    gpu/backend/present.cpp
 * @brief   End of frame: swapchain acquire, guest image scan-out, overlay,
 *          submit and present.
 */
#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include "renut_engine/gpu/sasrt_gpu_round.h"  // sasrt-nx (2026-11-08): the GPU round
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>

#include <plume_render_interface.h>

#include <rex/cvar.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/gpu_profile.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/render/textures.h"
#include "renut_engine/gpu/log.h"
#if defined(PLUME_DEKO3D)
#include "renut_engine/gpu/banjo_renut_deko3d.h"  // sasrt-nx (2026-11-08, sasrt_deko3d_diag_trace)
#endif
#include "renut_engine/gpu/overlay/overlay_ui.h"
#include "renut_engine/gpu/overlay/overlay_drawer.h"  // banjo-nx (2026-10-19): HasLatest
#include "renut_engine/gpu/banjo_renut_tuning.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/backend/video_internal.h"
#include "renut_engine/gpu/banjo_renut_capture.h"
#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>  // banjo-nx: banjo_renut_capture_raw's format check
#endif

// banjo-nx (2026-10-17, deko3d renderer tests): the captures copy the game's frame texture as it is (4-byte formats only,
// its channels as BGRA), without the scan-out pass: which of the two a wrong capture comes from.
REXCVAR_DEFINE_BOOL(banjo_renut_capture_raw, false, "Banjo/Renut",
                    "Test: frame captures copy the game's frame texture itself instead of the scanned-out picture "
                    "(deko3d builds, 4-byte formats)");

// banjo-nx (2026-10-18, the deko3d depth of field): one of the game's textures on screen and in the captures in place of
// the frame, to compare a pass's inputs between the back ends (the [trace] resolve lines name the headers).
REXCVAR_DEFINE_STRING(banjo_renut_diag_show_texture, "", "Banjo/Renut",
                      "Diagnostic: show the host texture of this guest texture header (hex address, as the [trace] "
                      "resolve lines name them) instead of the frame, on screen and in captures");
REXCVAR_DEFINE_DOUBLE(banjo_renut_diag_show_gamma, 1.0, "Banjo/Renut",
                      "Diagnostic: banjo_renut_diag_show_texture's exponent (depth near 1 shows with 64 or so)");

// banjo-nx (2026-10-20): see GameGamma below.
REXCVAR_DEFINE_BOOL(banjo_renut_game_gamma, false, "Banjo/Renut",
                    "Apply the gamma ramp the game loads to the output, as the Xbox 360's display and banjo-nx's own "
                    "renderer do (N&B's darkens the midtones, about x^1.17). Off: Renut's output as before");

REXCVAR_DEFINE_BOOL(renut_vsync, true, "Nuts&Bolts/Graphics",
                    "Show each frame at the display's refresh: no tearing, and the game runs at "
                    "the refresh rate (frame_cap then only limits below it). Off shows frames at "
                    "once, with tearing.");

REXCVAR_DEFINE_BOOL(vivapinata_present_lut, true, "Banjo/Enhancements",
                    "vivapinata-nx (2026-11-12, the GPU round; A/B 89): the scan-out with the game's gamma ramp "
                    "(banjo_renut_game_gamma), when it loads the frame's texels at one to one with no pow and the frame is "
                    "8-bit, reads the ramp from a 256 x 256 table of its own entries (two fetches a pixel) instead of the "
                    "ramp texture (three): each channel's entry depends on its 8-bit value alone. The same values: same "
                    "pixels. Off: the three fetches")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(vivapinata_present_lut_check, false, "Banjo/Diagnostics",
                    "vivapinata-nx (2026-11-12): a capture (banjo_renut_capture) that went through the gamma table "
                    "(vivapinata_present_lut) is drawn again through the plain scan-out and the two compared byte for "
                    "byte; the log says how many pixels differ")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace renut::gpu {

namespace {

// renut_vsync onto the swapchain (both backends start with it on). Vulkan
// makes the change by recreating the swapchain, which needsResize asks for.
void ApplyVsync(VideoState &s) {
  static bool applied = true;
  const bool vsync = REXCVAR_GET(renut_vsync);
  if (vsync == applied)
    return;
  s.swap_chain->setVsyncEnabled(vsync);
  applied = vsync;
  RGPU_INFO("Vsync {}", vsync ? "on" : "off");
}

void RebuildSwapChain(VideoState &s) {
  // Submit rather than abandon an open list: plume tracks layouts on record.
  if (s.list_open)
    detail::SubmitAndAdvance(s, nullptr, nullptr);
  detail::WaitAllSubmitted(s);
  s.swap_framebuffers.clear();
  if (!s.swap_chain->resize() || !detail::BuildSwapChainTargets(s)) {
    if (s.swap_chain->getWidth() && s.swap_chain->getHeight())
      RGPU_ERROR("Swapchain resize failed");  // 0x0 while minimized is expected
  }
}

// The most of the game's frame trimmed off either axis to fill the output.
constexpr float kMaxTrim = 0.15f;

struct FrameFit {
  plume::RenderViewport viewport;
  float visible[2] = {1.0f, 1.0f};  // the share of the frame shown on each axis
};

// The frame fills the output (a window that is not the frame's shape, say a
// maximized one) without bars: it covers it, keeping its proportions, and the
// part that overflows is trimmed evenly from both edges. Past kMaxTrim, the
// rest is fitted with bars.
FrameFit FitFrame(uint32_t out_w, uint32_t out_h, uint32_t src_w, uint32_t src_h) {
  FrameFit fit;
  fit.viewport = plume::RenderViewport(0.0f, 0.0f, float(out_w), float(out_h));
  if (src_w == 0 || src_h == 0 || out_w == 0 || out_h == 0)
    return fit;
  const float out_aspect = float(out_w) / float(out_h);
  float src_aspect = float(src_w) / float(src_h);
  if (src_aspect > out_aspect) {
    fit.visible[0] = std::max(out_aspect / src_aspect, 1.0f - kMaxTrim);
    src_aspect *= fit.visible[0];
  } else {
    fit.visible[1] = std::max(src_aspect / out_aspect, 1.0f - kMaxTrim);
    src_aspect /= fit.visible[1];
  }
  float w = float(out_w), h = float(out_h);
  if (src_aspect > out_aspect)
    h = w / src_aspect;
  else
    w = h * src_aspect;
  fit.viewport = plume::RenderViewport((float(out_w) - w) * 0.5f, (float(out_h) - h) * 0.5f, w, h);
  return fit;
}

// banjo-nx (2026-10-20, banjo_renut_game_gamma): the gamma ramp the game loads, which the 360's display applies to
// every 8-bit value it scans out (N&B's is about x^1.17: entries [64] = 192, [128] = 459, [255] = 1019 of 1023) and
// Renut's present never did. Off by default (it changes the picture); on, the present looks the ramp up by the value.
struct GameGamma {
  std::mutex mutex;
  // halo3-nx (2026-11-03): by the 10-bit value (NoteGameGamma expands either form the game loads): red, green, blue.
  std::array<uint16_t, 3 * 1024> ramp{};
  uint32_t version = 0;
  // Render thread.
  uint32_t uploaded = 0;
  uint32_t uploads = 0;  // kameo-nx (2026-11-03, banjo_renut_gamma_upload_once): copies of the ramp so far
  std::unique_ptr<plume::RenderTexture> texture;  // 1024 x 1, RGBA8 (kameo-nx: 8 bits; halo3-nx: 1024 entries)
  std::unique_ptr<plume::RenderTextureView> view;
  uint32_t slot = kInvalidDescriptor;
  float param = 0.0f;  // UtilityPush::param1 of the scan-outs: the slot as raw bits, 0 = no ramp
  // sasrt-nx (2026-11-08, sasrt_gpu_identity_ramp), in this renderer's 1024-entry form: every channel's entry i stores the
  // level entry i would store if it were i (the texels are level(entry)): the scan-out then computes level(v) for its
  // 10-bit value v without the three texture loads (present_ps.hlsl, param1 = 1). Exact for any input, not only 8-bit
  // ones. Ridge Racer 6's ramp is not one (entry 256 is 193): only for games that load the linear ramp.
  bool identity = false;
  uint32_t identity_version = 0;  // the ramp version 'identity' was worked out for
  // vivapinata-nx (2026-11-12, vivapinata_present_lut): the ramp by 8-bit value as a 256 x 256 RGBA8 table, texel (i, j)
  // = (red's entry for i, green's for j, blue's for i), made with the ramp's texels and uploaded with them.
  std::unique_ptr<plume::RenderTexture> lut_texture;
  std::unique_ptr<plume::RenderTextureView> lut_view;
  uint32_t lut_slot = kInvalidDescriptor;
  uint32_t lut_version = 0;  // the ramp version the table holds (0: none)
};
GameGamma g_gamma;
bool g_scanout_force_plain = false;  // vivapinata-nx (2026-11-12): vivapinata_present_lut_check's second drawing
bool g_scanout_used_lut = false;     // the last RecordScanOut went through the table

bool PresentLutOn() {
  static const bool on = REXCVAR_GET(vivapinata_present_lut);
  return on && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 89;
}

// vivapinata-nx (2026-11-12, vivapinata_present_lut): the 256 x 256 table present_lut_ps.hlsl reads, from the ramp's own
// texel values (the levels below are those of the ramp texture). present_ps's index for an 8-bit value n is
// round(n / 255 * 1023), which is (n * 2046 + 255) / 510: n * 1023 / 255 is never within 1 / 510 of a tie, far more than a
// float's error. Caller holds s.mutex, outside a pass.
void UploadGammaLutLocked(VideoState &s, const std::array<uint16_t, 3 * 1024> &ramp, uint32_t version) {
  constexpr plume::RenderFormat kFormat = plume::RenderFormat::R8G8B8A8_UNORM;
  if (!g_gamma.lut_texture) {
    g_gamma.lut_texture = s.device->createTexture(plume::RenderTextureDesc::Texture2D(256, 256, 1, kFormat));
    if (!g_gamma.lut_texture)
      return;
    g_gamma.lut_texture->setName("vivapinata game gamma table");
    g_gamma.lut_view = g_gamma.lut_texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(kFormat));
    g_gamma.lut_slot = g_gamma.lut_view ? Video::AllocateTextureSlotLocked() : kInvalidDescriptor;
    if (g_gamma.lut_slot == kInvalidDescriptor) {
      g_gamma.lut_view.reset();
      g_gamma.lut_texture.reset();
      return;
    }
    Video::SetTextureSlotLocked(g_gamma.lut_slot, g_gamma.lut_texture.get(), g_gamma.lut_view.get());
    g_gamma.lut_version = 0;
  }
  UploadAllocation staging = s.uploads[s.frame].Allocate(256 * 256 * 4, 512);
  if (!staging)
    return;
  const auto level = [](uint16_t entry) {
    return uint8_t((std::min<uint32_t>(entry, 1023u) * 510u + 1023u) / 2046u);
  };
  uint8_t r[256], g[256], b[256];
  for (uint32_t n = 0; n < 256; ++n) {
    const uint32_t v = (n * 2046u + 255u) / 510u;
    r[n] = level(ramp[v]);
    g[n] = level(ramp[1024 + v]);
    b[n] = level(ramp[2048 + v]);
  }
  auto *texels = static_cast<uint8_t *>(staging.cpu);
  for (uint32_t j = 0; j < 256; ++j) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint8_t *p = texels + (j * 256 + i) * 4;
      p[0] = r[i];
      p[1] = g[j];
      p[2] = b[i];
      p[3] = 255;
    }
  }
  plume::RenderCommandList *cmd = s.command_list;
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderTextureBarrier(g_gamma.lut_texture.get(), plume::RenderTextureLayout::COPY_DEST));
  cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(g_gamma.lut_texture.get(), 0, 0),
                         plume::RenderTextureCopyLocation::PlacedFootprint(staging.ref.ref, kFormat, 256, 256, 1, 256,
                                                                           staging.ref.offset),
                         0, 0, 0, nullptr);
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(g_gamma.lut_texture.get(), plume::RenderTextureLayout::SHADER_READ));
  g_gamma.lut_version = version;
}

// Before the frame's passes (a copy may not be recorded inside one). Caller holds s.mutex.
void PrepareGameGammaLocked(VideoState &s) {
  g_gamma.param = 0.0f;
  if (!REXCVAR_GET(banjo_renut_game_gamma))
    return;
  std::array<uint16_t, 3 * 1024> ramp;
  uint32_t version;
  {
    std::lock_guard lock(g_gamma.mutex);
    ramp = g_gamma.ramp;
    version = g_gamma.version;
  }
  if (!version || !Video::EnsureCommandListLocked())
    return;
  // kameo-nx (2026-11-03): 8 bits, not 32-bit floats: a quarter of the bytes for each of a pixel's three fetches. Each entry
  // is what the 8-bit swapchain would make of its float: n / 1023 is never nearer an 8-bit rounding tie than 0.0014 of a
  // level (checked for all 1024 values), so round() and the render output agree.
  constexpr plume::RenderFormat kFormat = plume::RenderFormat::R8G8B8A8_UNORM;
  if (!g_gamma.texture) {
    g_gamma.texture = s.device->createTexture(plume::RenderTextureDesc::Texture2D(1024, 1, 1, kFormat));
    if (!g_gamma.texture)
      return;
    g_gamma.texture->setName("banjo game gamma ramp");
    g_gamma.view = g_gamma.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(kFormat));
    g_gamma.slot = g_gamma.view ? Video::AllocateTextureSlotLocked() : kInvalidDescriptor;
    if (g_gamma.slot == kInvalidDescriptor) {
      g_gamma.view.reset();
      g_gamma.texture.reset();
      return;
    }
    Video::SetTextureSlotLocked(g_gamma.slot, g_gamma.texture.get(), g_gamma.view.get());
    g_gamma.uploaded = 0;
    g_gamma.uploads = 0;
  }
  // Every frame at first: a frame's list can be dropped at start-up (run 78's first upload never arrived: black output).
  // kameo-nx (2026-11-03, banjo_renut_gamma_upload_once): after that, when it changes. The copy, every frame, cost a full
  // barrier before it and a full barrier with an L2 invalidate after it, right before the scan-out reads the frame.
  const bool upload = !banjo::renut_gpu::GammaUploadOnceOn() || g_gamma.uploaded != version || g_gamma.uploads < 300;
  if (upload) {
    ++g_gamma.uploads;
    UploadAllocation staging = s.uploads[s.frame].Allocate(1024 * 4, 512);
    if (!staging)
      return;
    auto *texels = static_cast<uint8_t *>(staging.cpu);
    // round(entry * 255 / 1023); an entry over 1023 was over 1.0 as a float, which the 8-bit output clamps.
    const auto level = [](uint16_t entry) {
      return uint8_t((std::min<uint32_t>(entry, 1023u) * 510u + 1023u) / 2046u);
    };
    for (uint32_t i = 0; i < 1024; ++i) {
      texels[i * 4 + 0] = level(ramp[i]);
      texels[i * 4 + 1] = level(ramp[1024 + i]);
      texels[i * 4 + 2] = level(ramp[2048 + i]);
      texels[i * 4 + 3] = 255;
    }
    plume::RenderCommandList *cmd = s.command_list;
    cmd->barriers(plume::RenderBarrierStage::COPY,
                  plume::RenderTextureBarrier(g_gamma.texture.get(), plume::RenderTextureLayout::COPY_DEST));
    cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(g_gamma.texture.get(), 0, 0),
                           plume::RenderTextureCopyLocation::PlacedFootprint(staging.ref.ref, kFormat, 1024, 1, 1, 1024,
                                                                             staging.ref.offset),
                           0, 0, 0, nullptr);
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                  plume::RenderTextureBarrier(g_gamma.texture.get(), plume::RenderTextureLayout::SHADER_READ));
    if (REXCVAR_GET(vivapinata_present_lut))  // vivapinata-nx (2026-11-12): the table, with the ramp
      UploadGammaLutLocked(s, ramp, version);
    if (g_gamma.uploaded != version) {
      REXLOG_INFO("[renut] game gamma ramp applied (banjo_renut_game_gamma): [0]={}/{}/{} [64]={}/{}/{} [128]={}/{}/{} "
                  "[255]={}/{}/{} (of 1024)", ramp[0], ramp[1024], ramp[2048], ramp[256], ramp[1280], ramp[2304], ramp[512],
                  ramp[1536], ramp[2560], ramp[1023], ramp[2047], ramp[3071]);
    }
    g_gamma.uploaded = version;
  }
  g_gamma.param = std::bit_cast<float>(g_gamma.slot);  // the slot's bits (present_ps.hlsl); never 0, a dynamic slot
  // sasrt-nx (2026-11-08, sasrt_gpu_identity_ramp): worked out once a ramp version, as the texels are made above.
  if (g_gamma.identity_version != version) {
    g_gamma.identity_version = version;
    const auto level = [](uint32_t entry) { return (std::min<uint32_t>(entry, 1023u) * 510u + 1023u) / 2046u; };
    bool identity = true;
    for (uint32_t i = 0; i < 3 * 1024 && identity; ++i)
      identity = level(ramp[i]) == level(i % 1024);
    g_gamma.identity = identity;
  }
}

void RecordScanOut(VideoState &s, const PresentImage &image, uint32_t out_w, uint32_t out_h) {
  plume::RenderCommandList *cmd = s.command_list;
#if defined(PLUME_DEKO3D)
  if (IsDeko3D())
    banjo::renut_gpu::deko3d::TraceFrameBoundary();  // sasrt-nx (2026-11-08, sasrt_deko3d_diag_trace)
#endif
  if (image.layout && *image.layout != plume::RenderTextureLayout::SHADER_READ) {
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                  plume::RenderTextureBarrier(image.texture, plume::RenderTextureLayout::SHADER_READ));
    *image.layout = plume::RenderTextureLayout::SHADER_READ;
  }

  const uint32_t src_w = image.source_width ? image.source_width : image.width;
  const uint32_t src_h = image.source_height ? image.source_height : image.height;
  const FrameFit fit = FitFrame(out_w, out_h, src_w, src_h);
  const plume::RenderViewport &vp = fit.viewport;
  cmd->setViewports(vp);
  cmd->setScissors(plume::RenderRect(int32_t(vp.x), int32_t(vp.y), int32_t(vp.x + vp.width),
                                     int32_t(vp.y + vp.height)));

  cmd->setGraphicsPipelineLayout(s.utility_layout.get());

  UtilityPush push{};
  // The frame's share of the texture, less what the fit trims from its edges.
  const float frame_u = image.width ? float(src_w) / float(image.width) : 1.0f;
  const float frame_v = image.height ? float(src_h) / float(image.height) : 1.0f;
  push.scale[0] = frame_u * fit.visible[0];
  push.scale[1] = frame_v * fit.visible[1];
  push.translate[0] = frame_u * (1.0f - fit.visible[0]) * 0.5f;
  push.translate[1] = frame_v * (1.0f - fit.visible[1]) * 0.5f;
  push.texture_index = image.descriptor;
  push.sampler_index = kLinearClampSamplerSlot;
  // kameo-nx (2026-11-03, banjo_renut_present_texel_load): one texel for each output pixel, from the texture's corner: the
  // texel itself, which is what filtering at its centre gives. All ones in the sampler index (present_ps.hlsl).
  if (banjo::renut_gpu::PresentTexelLoadOn() && image.width == src_w && image.height == src_h && vp.x == 0.0f &&
      vp.y == 0.0f && vp.width == float(src_w) && vp.height == float(src_h) && fit.visible[0] == 1.0f &&
      fit.visible[1] == 1.0f)
    push.sampler_index = UINT32_MAX;
  push.param0 = image.gamma > 0.0f ? image.gamma : s.output_gamma;
  // kameo-nx (2026-11-03, banjo_renut_present_no_pow): the shader skips an exponent of exactly 1; a negative one is its size
  // with the pow kept (the key off).
  if (push.param0 == 1.0f && !banjo::renut_gpu::PresentNoPowOn())
    push.param0 = -1.0f;
  push.param1 = image.gamma > 0.0f ? 0.0f : g_gamma.param;  // banjo-nx: not on banjo_renut_diag_show_texture's views
  // sasrt-nx (2026-11-08, sasrt_gpu_identity_ramp; the second GPU round): an identity ramp's lookup computed instead: 1 in
  // param1's bits (slots 0-2 are the null textures, never the ramp's).
  if (push.param1 != 0.0f && g_gamma.identity && sasrt::gpu_round::On(sasrt::gpu_round::kIdentityRamp)) {
    push.param1 = std::bit_cast<float>(1u);
    sasrt::gpu_round::Count(sasrt::gpu_round::kIdentityRamp);
  }
  // vivapinata-nx (2026-11-12, vivapinata_present_lut): the ramp texture's lookups (param1 its slot, not 0 or the identity's
  // 1) of an 8-bit frame loaded at one to one with no pow: the table made from the same ramp version instead.
  plume::RenderPipeline *pipeline = s.present_pipeline.get();
  const uint32_t ramp_bits = std::bit_cast<uint32_t>(push.param1);
  g_scanout_used_lut = false;
  if (s.present_lut_pipeline && PresentLutOn() && !g_scanout_force_plain && image.unorm8 && push.sampler_index == UINT32_MAX &&
      push.param0 == 1.0f && ramp_bits > 1u && ramp_bits == g_gamma.slot && g_gamma.lut_version != 0 &&
      g_gamma.lut_version == g_gamma.uploaded && g_gamma.lut_slot != kInvalidDescriptor) {
    pipeline = s.present_lut_pipeline.get();
    push.param1 = std::bit_cast<float>(g_gamma.lut_slot);
    g_scanout_used_lut = true;
    static uint64_t lut_scans = 0, logged = 0;
    if (++lut_scans >= logged * 2 + 1) {  // the 1st, 3rd, 7th ... 2^k - 1th
      logged = lut_scans;
      RGPU_INFO("[renut] scan-out through the gamma table (vivapinata_present_lut): {} frames", lut_scans);
    }
  }
  cmd->setPipeline(pipeline);
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
}

// banjo-nx: the test captures (banjo_renut_capture.h). CaptureFrame asks; the next Present, minimized or not, scans
// the guest image out once more into an offscreen target of the frame's size and reads it back.
struct FrameCapture {
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic<uint64_t> requested{0};
  uint64_t served = 0;  // under mutex
  bool ok = false;
  rex::ui::RawImage image;
  // Render thread only. Kept between captures of one size (the recording takes one per frame); never in flight
  // between them, as each capture waits for the GPU.
  std::unique_ptr<plume::RenderTexture> target;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  std::unique_ptr<plume::RenderBuffer> readback;
  uint32_t width = 0, height = 0, row_texels = 0;
  uint64_t served_render = 0;
};
FrameCapture g_capture;

// The present pipeline's format (kSwapChainFormat in video.cpp).
constexpr plume::RenderFormat kCaptureFormat = plume::RenderFormat::B8G8R8A8_UNORM;

bool CaptureFrameLocked(VideoState &s, const PresentImage &image, rex::ui::RawImage &out) {
  const uint32_t w = image.source_width ? image.source_width : image.width;
  const uint32_t h = image.source_height ? image.source_height : image.height;
  if (!w || !h || !Video::EnsureCommandListLocked())
    return false;
  FrameCapture &c = g_capture;
  if (!c.target || c.width != w || c.height != h) {
    c.framebuffer.reset();
    c.target.reset();
    c.readback.reset();
    plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(w, h, 1, kCaptureFormat);
    desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
    c.target = s.device->createTexture(desc);
    if (!c.target)
      return false;
    const plume::RenderTexture *attachment = c.target.get();
    c.framebuffer = s.device->createFramebuffer(plume::RenderFramebufferDesc(&attachment, 1));
    c.row_texels = (w + 63) & ~63u;  // D3D12 copies rows of 256-byte multiples
    c.readback = s.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(uint64_t(c.row_texels) * h * 4));
    if (!c.framebuffer || !c.readback) {
      c.framebuffer.reset();
      c.target.reset();
      c.readback.reset();
      return false;
    }
    c.width = w;
    c.height = h;
  }
  plume::RenderCommandList *cmd = s.command_list;
  bool raw = false;
  bool raw_rgba = false;  // the frame texture's bytes are R G B A (else B G R A, as the scan-out's readback)
#if defined(PLUME_DEKO3D)
  if (REXCVAR_GET(banjo_renut_capture_raw) && IsDeko3D()) {
    const auto *t = static_cast<const plume::Deko3DTexture *>(image.texture);
    if (t && t->valid) {
      switch (t->desc.format) {
      case plume::RenderFormat::R8G8B8A8_TYPELESS:
      case plume::RenderFormat::R8G8B8A8_UNORM:
        raw = raw_rgba = true;
        break;
      case plume::RenderFormat::B8G8R8A8_UNORM:
        raw = true;
        break;
      default:
        break;
      }
    }
    static int logged = 0;
    if (t && logged++ < 4)
      RGPU_INFO("banjo_renut_capture_raw: frame texture {}x{} format {}{}", t->desc.width, t->desc.height,
                int(t->desc.format), raw ? "" : ": not 8-bit RGBA or BGRA, the scan-out is captured");
  }
#endif
  if (raw) {
    cmd->barriers(plume::RenderBarrierStage::COPY,
                  plume::RenderTextureBarrier(image.texture, plume::RenderTextureLayout::COPY_SOURCE));
    if (image.layout)
      *image.layout = plume::RenderTextureLayout::COPY_SOURCE;
    const plume::RenderBox box(0, 0, int32_t(w), int32_t(h));
    cmd->copyTextureRegion(
        plume::RenderTextureCopyLocation::PlacedFootprint(c.readback.get(), kCaptureFormat, w, h, 1, c.row_texels),
        plume::RenderTextureCopyLocation::Subresource(image.texture), 0, 0, 0, &box);
  } else {
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(c.target.get(), plume::RenderTextureLayout::COLOR_WRITE));
  cmd->setFramebuffer(c.framebuffer.get());
  RecordScanOut(s, image, w, h);
  cmd->setFramebuffer(nullptr);
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderTextureBarrier(c.target.get(), plume::RenderTextureLayout::COPY_SOURCE));
  cmd->copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(c.readback.get(), kCaptureFormat, w, h, 1, c.row_texels),
      plume::RenderTextureCopyLocation::Subresource(c.target.get()));
  }
  // Submits the frame's work with the capture and waits; the next list starts with nothing bound.
  detail::SubmitAndWaitLocked(s);
  const auto *data = static_cast<const uint8_t *>(c.readback->map());
  if (!data)
    return false;
  out.width = w;
  out.height = h;
  out.stride = size_t(w) * 4;
  out.data.resize(size_t(w) * h * 4);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t *row = data + size_t(y) * c.row_texels * 4;
    uint8_t *dest = out.data.data() + size_t(y) * w * 4;
    // B G R A -> R G B X; a raw RGBA frame texture is in order already (before 2026-10-17 it was swapped too: every
    // raw capture of the Ryujinx runs had red and blue exchanged, the screen itself did not).
    const uint32_t r = raw_rgba ? 0 : 2, b = raw_rgba ? 2 : 0;
    for (uint32_t x = 0; x < w; ++x) {
      dest[x * 4 + 0] = row[x * 4 + r];
      dest[x * 4 + 1] = row[x * 4 + 1];
      dest[x * 4 + 2] = row[x * 4 + b];
      dest[x * 4 + 3] = 0xFF;
    }
  }
  c.readback->unmap();
  // vivapinata-nx (2026-11-12, vivapinata_present_lut_check): the same frame through the plain scan-out (present_ps's
  // three ramp fetches), compared with the table's drawing byte for byte.
  if (!raw && g_scanout_used_lut && REXCVAR_GET(vivapinata_present_lut_check) && Video::EnsureCommandListLocked()) {
    plume::RenderCommandList *check = s.command_list;
    check->barriers(plume::RenderBarrierStage::GRAPHICS,
                    plume::RenderTextureBarrier(c.target.get(), plume::RenderTextureLayout::COLOR_WRITE));
    check->setFramebuffer(c.framebuffer.get());
    g_scanout_force_plain = true;
    RecordScanOut(s, image, w, h);
    g_scanout_force_plain = false;
    check->setFramebuffer(nullptr);
    check->barriers(plume::RenderBarrierStage::COPY,
                    plume::RenderTextureBarrier(c.target.get(), plume::RenderTextureLayout::COPY_SOURCE));
    check->copyTextureRegion(
        plume::RenderTextureCopyLocation::PlacedFootprint(c.readback.get(), kCaptureFormat, w, h, 1, c.row_texels),
        plume::RenderTextureCopyLocation::Subresource(c.target.get()));
    detail::SubmitAndWaitLocked(s);
    if (const auto *plain = static_cast<const uint8_t *>(c.readback->map())) {
      uint64_t differ = 0;
      int largest = 0;
      for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *row = plain + size_t(y) * c.row_texels * 4;
        const uint8_t *lut = out.data.data() + size_t(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
          const int d[3] = {row[x * 4 + 2] - lut[x * 4 + 0], row[x * 4 + 1] - lut[x * 4 + 1], row[x * 4 + 0] - lut[x * 4 + 2]};
          if (d[0] || d[1] || d[2]) {
            ++differ;
            for (int k : d)
              largest = std::max(largest, std::abs(k));
          }
        }
      }
      c.readback->unmap();
      RGPU_INFO("[renut] gamma table check (vivapinata_present_lut_check): {} of {} pixels differ from the plain "
                "scan-out (largest {} levels)", differ, uint64_t(w) * h, largest);
    }
  }
  return true;
}

// From PresentLocked: one relaxed load when nobody asked.
void ServeCaptureLocked(VideoState &s, const PresentImage *image) {
  FrameCapture &c = g_capture;
  const uint64_t requested = c.requested.load(std::memory_order_acquire);
  if (requested == c.served_render || !image || !image->texture || image->descriptor == kInvalidDescriptor)
    return;
  rex::ui::RawImage out;
  const bool ok = CaptureFrameLocked(s, *image, out);
  c.served_render = requested;
  {
    std::lock_guard lock(c.mutex);
    c.image = std::move(out);
    c.ok = ok;
    c.served = requested;
  }
  c.cv.notify_all();
}

// banjo_renut_diag_show_texture: the texture to show instead of the frame, when it has a 2D host texture and slot.
bool DiagTextureImageLocked(PresentImage &out) {
  static const u32 va = [] {
    const std::string v = REXCVAR_GET(banjo_renut_diag_show_texture);
    return v.empty() ? 0u : u32(std::strtoul(v.c_str(), nullptr, 16));
  }();
  if (!va)
    return false;
  HostTexture *t = FindHostTextureLocked(va);
  if (!t || !t->texture || t->slot == UINT32_MAX || t->slot == kInvalidDescriptor ||
      t->dimension != TextureDimension::Tex2D || t->stacked)
    return false;
  out.texture = t->texture.get();
  out.layout = &t->layout;
  out.descriptor = t->slot;
  out.width = t->alloc_width;
  out.height = t->alloc_height;
  out.gamma = float(REXCVAR_GET(banjo_renut_diag_show_gamma));
  return true;
}

// Records the back-buffer pass and presents. Caller holds s.mutex.
void PresentLocked(VideoState &s, const PresentImage *image) {
  // Logs, once, why the GPU stopped if an earlier submission faulted.
  ReportDeviceLost(s.device.get());
  PresentImage diag_image;
  if (image && DiagTextureImageLocked(diag_image))
    image = &diag_image;
  ApplyVsync(s);
  if (s.swap_chain->needsResize() || s.resize_requested.exchange(false, std::memory_order_acq_rel))
    RebuildSwapChain(s);
  PrepareGameGammaLocked(s);     // banjo-nx (2026-10-20, banjo_renut_game_gamma)
  ServeCaptureLocked(s, image);  // banjo-nx: test captures (banjo_renut_capture.h)
  if (s.swap_framebuffers.empty()) {
    // Minimized or failed resize: keep the frame ring moving without a present.
    if (s.list_open)
      detail::SubmitAndAdvance(s, nullptr, nullptr);
    return;
  }

  plume::RenderCommandSemaphore *acquire = s.acquire_semaphores[s.frame].get();
  uint32_t image_index = 0;
  auto acquire_image = [&] {
    render_thread::WaitScope wait;
    return s.swap_chain->acquireTexture(acquire, &image_index);
  };
  if (!acquire_image()) {
    RebuildSwapChain(s);
    acquire = s.acquire_semaphores[s.frame].get();
    if (s.swap_framebuffers.empty() || !acquire_image()) {
      if (s.list_open)
        detail::SubmitAndAdvance(s, nullptr, nullptr);
      return;
    }
  }

  if (!Video::EnsureCommandListLocked())
    return;
  static const uint32_t kProfileLabel = gpu_profile::Label("present and overlays");
  gpu_profile::Mark(s, kProfileLabel);
  plume::RenderCommandList *cmd = s.command_list;
  plume::RenderTexture *back = s.swap_chain->getTexture(image_index);
  plume::RenderFramebuffer *back_fb = s.swap_framebuffers[image_index].get();
  const uint32_t out_w = s.swap_chain->getWidth();
  const uint32_t out_h = s.swap_chain->getHeight();

  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(back, plume::RenderTextureLayout::COLOR_WRITE));
  cmd->setFramebuffer(back_fb);
  const bool scan_out = image && image->texture && image->descriptor != kInvalidDescriptor;
  // Letterbox bars (and the whole output when there is no guest image). banjo-nx (2026-10-19, GPU): not when the
  // frame covers the whole output, where the scan-out writes every pixel the clear would.
  bool covers = false;
  if (scan_out) {
    const FrameFit fit = FitFrame(out_w, out_h, image->source_width ? image->source_width : image->width,
                                  image->source_height ? image->source_height : image->height);
    covers = fit.viewport.x <= 0.0f && fit.viewport.y <= 0.0f && fit.viewport.x + fit.viewport.width >= float(out_w) &&
             fit.viewport.y + fit.viewport.height >= float(out_h);
  }
  if (!covers)
    cmd->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));

  if (scan_out)
    RecordScanOut(s, *image, out_w, out_h);

  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(out_w), float(out_h)));
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(out_w), int32_t(out_h)));
  // The SDK's own menus, then reNut's overlay over them.
  if (s.overlay_hook)
    s.overlay_hook(cmd, back_fb, out_w, out_h);
  overlay::DrawLocked(cmd, out_w, out_h);

  cmd->setFramebuffer(nullptr);
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));

  plume::RenderCommandSemaphore *present_semaphore = s.present_semaphores[image_index].get();
  detail::SubmitAndAdvance(s, acquire, present_semaphore);
  plume::RenderCommandSemaphore *waits[] = {present_semaphore};
  bool presented;
  {
    // With vsync the present can hold until the display takes a frame.
    render_thread::WaitScope wait;
    presented = s.swap_chain->present(image_index, waits, 1);
  }
  if (!presented)
    s.resize_requested.store(true, std::memory_order_release);
}

} // namespace

namespace {

// A guest that has not presented for this long is stalled (loading, movie
// decode); the UI pump then presents the overlay on its own.
constexpr int64_t kGuestStallMs = 100;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

// banjo-nx (2026-10-19, latency batch 3): the fences of the last guest presents' frames, as copies (the slot's fence is
// re-armed two frames later), for the display thread (renut_engine/display_sync.cpp) to test without waiting. Its own
// mutex: the render thread holds State().mutex for whole frames.
namespace {
constexpr uint32_t kPresentFences = 8;
struct PresentFence {
  uint64_t index = 0;
#if defined(PLUME_DEKO3D)
  plume::Deko3DFenceCopy fence;
#endif
};
std::mutex g_present_fence_mutex;
PresentFence g_present_fences[kPresentFences];
std::atomic<uint64_t> g_present_count{0};
} // namespace

uint64_t Video::PresentCount() {
  return g_present_count.load(std::memory_order_acquire);
}

int Video::PresentDone(uint64_t index) {
#if defined(PLUME_DEKO3D)
  if (!index || index > g_present_count.load(std::memory_order_acquire))
    return -1;
  plume::Deko3DFenceCopy fence;
  {
    // Copied out: the test (an ioctl) runs without the mutex the render thread takes at every present.
    std::lock_guard lock(g_present_fence_mutex);
    const PresentFence &p = g_present_fences[index % kPresentFences];
    if (p.index != index)
      return 1;  // older than the ring: long finished (two frames in flight)
    fence = p.fence;
  }
  return plume::Deko3DFenceCopyDone(fence) ? 1 : 0;
#else
  (void)index;
  return -1;
#endif
}

void Video::NoteGameGamma(const uint8_t *ramp, bool piecewise) {
  if (!ramp || !REXCVAR_GET(banjo_renut_game_gamma))
    return;
  const auto word = [ramp](uint32_t i) { return (uint32_t(ramp[i * 2]) << 8 | ramp[i * 2 + 1]) >> 6 & 0x3FF; };
  std::array<uint16_t, 3 * 1024> r;
  for (uint32_t c = 0; c < 3; ++c) {
    for (uint32_t i = 0; i < 1024; ++i) {
      // halo3-nx (2026-11-03): D3DDevice_SetPWLGamma's form is 128 { base, delta } segments of a channel (Halo 3's):
      // base + delta * the value's low 3 bits / 8. D3DDevice_SetGammaRamp's is 256 entries, by the 8-bit value.
      r[c * 1024 + i] = piecewise ? uint16_t(std::min(word(c * 256 + (i >> 3) * 2) + word(c * 256 + (i >> 3) * 2 + 1) * (i & 7) / 8, 1023u))
                                  : uint16_t(word(c * 256 + (i * 255 + 511) / 1023));
    }
  }
  std::lock_guard lock(g_gamma.mutex);
  if (r != g_gamma.ramp || !g_gamma.version) {
    g_gamma.ramp = r;
    ++g_gamma.version;
  }
}

void Video::Present(const PresentImage *image) {
  auto &s = State();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::lock_guard lock(s.mutex);
  if (!s.host_ready || s.shutting_down.load(std::memory_order_acquire))
    return;
  PresentLocked(s, image);
  s.last_guest_present_ms.store(NowMs(), std::memory_order_release);
  detail::LatencyTrimLocked(s);  // banjo-nx (2026-10-20, input latency, banjo_deko3d_latency_trim)
  {
    // The frame just submitted: the slot before the one SubmitAndAdvance moved to.
    std::lock_guard fence_lock(g_present_fence_mutex);
    const uint64_t index = g_present_count.load(std::memory_order_relaxed) + 1;
    PresentFence &p = g_present_fences[index % kPresentFences];
    p.index = index;
#if defined(PLUME_DEKO3D)
    p.fence = plume::Deko3DCopyFence(s.fences[(s.frame + kFramesInFlight - 1) % kFramesInFlight].get());
#endif
    g_present_count.store(index, std::memory_order_release);
  }
}

void Video::PresentOverlayOnly() {
  auto &s = State();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  const int64_t now = NowMs();
  if (s.guest_ready &&
      now - s.last_guest_present_ms.load(std::memory_order_acquire) < kGuestStallMs)
    return;
  // The render thread holds the mutex while it waits on the UI thread for the
  // overlay, so never block on it here.
  std::unique_lock lock(s.mutex, std::try_to_lock);
  if (!lock.owns_lock() || !s.host_ready || s.shutting_down.load(std::memory_order_acquire))
    return;
  // banjo-nx (2026-10-19, the colour flash): while the guest stalls (loading) with nothing of the overlay to show,
  // nothing is presented: the display keeps the last frame, as the console's does. The re-present of the last front
  // buffer showed whatever that texture held by then, and the game may free or remake it while it loads: on deko3d a
  // new image's memory is not cleared (Vulkan's came zeroed), a full-screen frame of other data. With the overlay up
  // the last frame is presented under it, but only from a texture a resolve wrote (else the overlay on black).
  // banjo_renut_stall_present = true: as before.
  if (s.guest_ready && !banjo::renut_gpu::StallPresentOn() && !OverlayDrawer::HasLatest())
    return;
  PresentImage image =
      s.guest_ready && s.last_front_buffer ? FrontBufferImageLocked(s.last_front_buffer) : PresentImage{};
  if (image.texture && !banjo::renut_gpu::StallPresentOn() && !FrontBufferWrittenLocked(s.last_front_buffer))
    image = PresentImage{};
  PresentLocked(s, image.texture ? &image : nullptr);
}

bool CaptureFrame(rex::ui::RawImage &out, int timeout_ms) {
  FrameCapture &c = g_capture;
  std::unique_lock lock(c.mutex);
  const uint64_t ticket = c.requested.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (!c.cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return c.served >= ticket; }) || !c.ok)
    return false;
  out = c.image;
  return true;
}

} // namespace renut::gpu
