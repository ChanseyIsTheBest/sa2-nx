// banjo-nx (2026-10-21, internal resolution, the user's request: 648p handheld, 900p docked, the HUD at the display's
// resolution; sa2-nx and Peggle 2 switch their resolution by the console's mode too).
//
// An emulator trace of a gameplay frame: the 3D scene into a 1280x720 target, depth of field into a second one, the bloom
// chain, the final composite (pixel shader 981A6A58F2ABFA42) into that second target, then the HUD over it (alpha-blended
// quads without the depth or stencil test) and its resolve to the front buffer. Every target of the frame is drawn at the
// internal resolution (surfaces.cpp, LatchResolutionScaleLocked; the post-processing buffers and shadow maps keep the
// console's sizes); right after the composite and SMAA (render/smaa.cpp) one full-screen pass scales the composite's
// target into its UI target at the display's resolution (shaders/host/ui_upscale_ps.hlsl: bilinear, one sample a pixel,
// the user's choice in every config; ui_upscale_bicubic_ps.hlsl with banjo_renut_upscale_filter 1), and the rest of the
// frame's lookups of that target answer the UI target
// (RedirectToUiSurfaceLocked): the HUD, its clears and the resolve to the front buffer, which the present shows as it is.
// Recorded like render/smaa.cpp: the utility layout, its bindless tables and push block.
#include "renut_engine/gpu/render/ui_scale.h"

#include <memory>
#include <unordered_map>

#include <plume_render_interface.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/gpu_profile.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/surfaces.h"

#include "host_shaders/smaa_vs.spirv.h"
#include "host_shaders/ui_depth_upscale_ps.spirv.h"
#include "host_shaders/ui_upscale_bicubic_ps.spirv.h"
#include "host_shaders/ui_upscale_ps.spirv.h"
#if defined(PLUME_DEKO3D)
#include "host_shaders/smaa_vs.deko3d.h"
#include "host_shaders/ui_depth_upscale_ps.deko3d.h"
#include "host_shaders/ui_upscale_bicubic_ps.deko3d.h"
#include "host_shaders/ui_upscale_ps.deko3d.h"
#endif
#if defined(_WIN32)
#include "host_shaders/smaa_vs.dxil.h"
#include "host_shaders/ui_depth_upscale_ps.dxil.h"
#include "host_shaders/ui_upscale_bicubic_ps.dxil.h"
#include "host_shaders/ui_upscale_ps.dxil.h"
#endif

namespace renut::gpu {
namespace {

struct UiScalePass {
  bool failed = false;  // a shader could not be made: the HUD stays at the internal resolution
  std::unique_ptr<plume::RenderShader> vs, ps[2];  // bilinear, bicubic (banjo_renut_upscale_filter)
  std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> pipelines;  // by the target's format and filter
  // The UI target's framebuffer, for the surface and generation it was made for.
  const HostSurface *target = nullptr;
  uint64_t target_generation = 0;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  // The depth pass (UiDepthAfterCompositeLocked): its shader, pipelines by the depth format, framebuffer.
  std::unique_ptr<plume::RenderShader> depth_ps;
  std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> depth_pipelines;
  const HostSurface *depth_target = nullptr;
  uint64_t depth_generation = 0;
  std::unique_ptr<plume::RenderFramebuffer> depth_framebuffer;
  bool depth_failed = false;
};
UiScalePass g_pass;

plume::RenderPipeline *Pipeline(VideoState &s, plume::RenderFormat format, uint32_t filter) {
  UiScalePass &p = g_pass;
  if (!p.vs) {
    p.vs = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(smaa_vs), "smaa_vs");
    p.ps[0] = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(ui_upscale_ps), "ui_upscale_ps");
    p.ps[1] = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(ui_upscale_bicubic_ps), "ui_upscale_bicubic_ps");
    if (!p.vs || !p.ps[0] || !p.ps[1]) {
      p.failed = true;
      RGPU_WARN("[renut] the upscale's shaders could not be made: the HUD stays at the internal resolution");
      return nullptr;
    }
  }
  auto &pipeline = p.pipelines[uint32_t(format) << 1 | filter];
  if (!pipeline) {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = s.utility_layout.get();
    desc.vertexShader = p.vs.get();
    desc.pixelShader = p.ps[filter].get();
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = format;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
    pipeline = s.device->createGraphicsPipeline(desc);
    if (!pipeline) {
      p.failed = true;
      RGPU_WARN("[renut] the upscale's pipeline could not be made: the HUD stays at the internal resolution");
      return nullptr;
    }
    pipeline->setName("renut UI upscale");
  }
  return pipeline.get();
}

plume::RenderPipeline *DepthPipeline(VideoState &s, plume::RenderFormat format) {
  UiScalePass &p = g_pass;
  if (!p.depth_ps) {
    if (!p.vs)
      p.vs = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(smaa_vs), "smaa_vs");
    p.depth_ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(ui_depth_upscale_ps), "ui_depth_upscale_ps");
    if (!p.vs || !p.depth_ps) {
      p.depth_failed = true;
      RGPU_WARN("[renut] the UI depth pass's shaders could not be made");
      return nullptr;
    }
  }
  auto &pipeline = p.depth_pipelines[uint32_t(format)];
  if (!pipeline) {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = s.utility_layout.get();
    desc.vertexShader = p.vs.get();
    desc.pixelShader = p.depth_ps.get();
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 0;
    desc.depthTargetFormat = format;
    desc.depthEnabled = true;
    desc.depthWriteEnabled = true;
    desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
    pipeline = s.device->createGraphicsPipeline(desc);
    if (!pipeline) {
      p.depth_failed = true;
      RGPU_WARN("[renut] the UI depth pass's pipeline could not be made");
      return nullptr;
    }
    pipeline->setName("renut UI depth upscale");
  }
  return pipeline.get();
}

}  // namespace

HostSurface *UiDepthAfterCompositeLocked(HostSurface *depth) {
  VideoState &s = State();
  UiScalePass &p = g_pass;
  if (p.depth_failed || !UiRedirectedLocked() || !depth || !depth->depth || !depth->texture ||
      depth->samples != plume::RenderSampleCount::COUNT_1 || depth->scale == UiScale() || !s.command_list)
    return nullptr;
  plume::RenderPipeline *pipeline = DepthPipeline(s, depth->format);
  if (!pipeline)
    return nullptr;
  HostSurface *ui = UiDepthSurfaceLocked(depth);
  const uint32_t source_slot = SurfaceSampleSlotLocked(depth);
  if (!ui || source_slot == kInvalidDescriptor)
    return nullptr;
  if (p.depth_target != ui || p.depth_generation != ui->generation || !p.depth_framebuffer) {
    p.depth_framebuffer = s.device->createFramebuffer(plume::RenderFramebufferDesc(nullptr, 0, ui->texture.get()));
    p.depth_target = ui;
    p.depth_generation = ui->generation;
    if (!p.depth_framebuffer)
      return nullptr;
  }
  static const uint32_t kLabel = gpu_profile::Label("UI depth upscale");
  gpu_profile::Mark(s, kLabel);
  plume::RenderCommandList *cmd = s.command_list;
  plume::RenderTextureBarrier barriers[2];
  uint32_t count = 0;
  if (depth->layout != plume::RenderTextureLayout::SHADER_READ) {
    barriers[count++] = plume::RenderTextureBarrier(depth->texture.get(), plume::RenderTextureLayout::SHADER_READ);
    depth->layout = plume::RenderTextureLayout::SHADER_READ;
  }
  if (ui->layout != plume::RenderTextureLayout::DEPTH_WRITE) {
    barriers[count++] = plume::RenderTextureBarrier(ui->texture.get(), plume::RenderTextureLayout::DEPTH_WRITE);
    ui->layout = plume::RenderTextureLayout::DEPTH_WRITE;
  }
  if (count)
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0, barriers, count);
  const uint32_t source_width = depth->scale.Size(depth->width);
  const uint32_t source_height = depth->scale.Size(depth->height);
  const uint32_t width = ui->scale.Size(ui->width);
  const uint32_t height = ui->scale.Size(ui->height);
  UtilityPush push{};
  push.scale[0] = 1.0f / float(source_width);
  push.scale[1] = 1.0f / float(source_height);
  push.translate[0] = float(source_width);
  push.translate[1] = float(source_height);
  push.texture_index = source_slot;
  push.sampler_index = kPointClampSamplerSlot;
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  cmd->setFramebuffer(p.depth_framebuffer.get());
  // The stencil is not the scene's (a shader cannot write it): 0, as cleared.
  if (plume::RenderFormatIsStencil(ui->format))
    cmd->clearDepthStencil(false, true, 0.0f, 0);
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));
  cmd->setPipeline(pipeline);
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  ++ui->writes;
  ui->write_seq = ++g_surface_write_seq;  // blur-nx (2026-11-18): written whole now (as the colour pass below)
  RedirectDepthToUiLocked();
  banjo::renut_gpu::CountUiDepth(true);
  return ui;
}

bool UiScaleAfterCompositeLocked(HostSurface *surface) {
  NoteCompositeLocked();
  VideoState &s = State();
  if (g_pass.failed || !surface || surface->depth || !surface->texture ||
      surface->samples != plume::RenderSampleCount::COUNT_1 || surface->scale == UiScale() || UiRedirectedLocked() ||
      !s.command_list) {
    if (surface && surface->scale == UiScale())
      banjo::renut_gpu::CountUiUpscale(false);
    return false;
  }
  const uint32_t filter = banjo::renut_gpu::UpscaleFilter() ? 1u : 0u;
  plume::RenderPipeline *pipeline = Pipeline(s, surface->format, filter);
  if (!pipeline)
    return false;
  HostSurface *ui = UiSurfaceLocked(surface);
  const uint32_t source_slot = SurfaceSampleSlotLocked(surface);
  if (!ui || source_slot == kInvalidDescriptor)
    return false;
  UiScalePass &p = g_pass;
  if (p.target != ui || p.target_generation != ui->generation || !p.framebuffer) {
    const plume::RenderTexture *colors[1] = {ui->texture.get()};
    p.framebuffer = s.device->createFramebuffer(plume::RenderFramebufferDesc(colors, 1, nullptr));
    p.target = ui;
    p.target_generation = ui->generation;
    if (!p.framebuffer)
      return false;
  }
  static const uint32_t kLabel = gpu_profile::Label("UI upscale");
  gpu_profile::Mark(s, kLabel);
  plume::RenderCommandList *cmd = s.command_list;
  // One barrier for both (plume's deko3d back end merges a call's transitions). The UI target is not cleared: the pass
  // writes every pixel, without blending and with the whole write mask (a partial one would read the target back).
  plume::RenderTextureBarrier barriers[2];
  uint32_t count = 0;
  if (surface->layout != plume::RenderTextureLayout::SHADER_READ) {
    barriers[count++] = plume::RenderTextureBarrier(surface->texture.get(), plume::RenderTextureLayout::SHADER_READ);
    surface->layout = plume::RenderTextureLayout::SHADER_READ;
  }
  if (ui->layout != plume::RenderTextureLayout::COLOR_WRITE) {
    barriers[count++] = plume::RenderTextureBarrier(ui->texture.get(), plume::RenderTextureLayout::COLOR_WRITE);
    ui->layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  if (count)
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0, barriers, count);

  const uint32_t source_width = surface->scale.Size(surface->width);
  const uint32_t source_height = surface->scale.Size(surface->height);
  const uint32_t width = ui->scale.Size(ui->width);
  const uint32_t height = ui->scale.Size(ui->height);
  UtilityPush push{};
  push.scale[0] = 1.0f / float(source_width);
  push.scale[1] = 1.0f / float(source_height);
  push.translate[0] = float(source_width);
  push.translate[1] = float(source_height);
  push.texture_index = source_slot;
  push.sampler_index = kLinearClampSamplerSlot;
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  cmd->setFramebuffer(p.framebuffer.get());
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));
  cmd->setPipeline(pipeline);
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);

  ++ui->writes;  // banjo_renut_resolve_skip: the target changed
  // blur-nx (2026-11-18, the band at the top of the race at any internal height but the display's): the pass writes the
  // whole UI target, so it is written now, after every other view of its EDRAM. Without this its write_seq stayed at the
  // last frame's HUD, and the HUD's first blended draw (InheritEdramBeforeDrawLocked, blur_renut_edram_inherit) took the
  // post-processing buffers drawn over the same tiles earlier in the frame (1024x512, 512x256, 320x180 at base 000; at
  // the console's scale, as the UI target is in handheld mode) over its top rows. On the console the composite is the
  // newest write there. At the display's height no UI target is used, so 720 showed nothing.
  ui->write_seq = ++g_surface_write_seq;
  g_blur_color_write_last = ui->write_seq;
  ui->blur_inherit_checked = g_blur_color_write_last;
  RedirectToUiSurfaceLocked();
  banjo::renut_gpu::CountUiUpscale(true);
  return true;
}

}  // namespace renut::gpu
