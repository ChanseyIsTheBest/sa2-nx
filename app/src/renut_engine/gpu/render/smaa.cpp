// banjo-nx (2026-10-21, banjo_renut_smaa, the user's request): SMAA 1x on the frame's final composite.
//
// N&B draws its 3D image, then depth of field and bloom, then one full-screen composite into a 1280x720 8-bit target
// (pixel shader 981A6A58F2ABFA42 in gameplay), then the HUD over it (minimap, icons, text: alpha-blended draws), and
// resolves that target to the front buffer. SMAA runs right after the composite draw (banjo_renut_smaa_after_ps): the
// whole 3D image is there and post-processed (tone-mapped, gamma space: what SMAA's luma edge detection is made for),
// and nothing of the HUD yet. Three passes (shaders/host/smaa*.hlsl, Jimenez et al.'s SMAA.hlsl, MIT):
//   1. edges: the target read once a pixel; out come the luma edges (R8G8), a copy of the pixel ('src', which the blend
//      pass reads while it writes the target) and a depth mask (D16): 1 on edge pixels, 0.5 on pixels the blend pass
//      can change without an edge of their own, 0 elsewhere (smaa_edge_ps.hlsl says why that is exact);
//   2. blending weights into 'weights' (RGBA8, cleared to 0): depth test LESS at 0.75, so only the edge pixels run the
//      shader (its result is 0 everywhere else, which the clear gives);
//   3. neighbourhood blending of src with the weights into the target, depth test LESS at 0.25: only the pixels it can
//      change; the others keep what the composite wrote, which is what the blend would write there. Red, green and blue
//      (the target keeps its alpha, which the composite does not write either).
// One barrier before each pass (plume's deko3d back end merges the transitions of one point into one). The textures are
// made at the target's size on first use (and again when it changes); the area and search textures are uploaded once.
// Recorded like DrawResolveCopy: the utility layout, its bindless tables and push block; the caller binds the game's
// targets and state again.
// History (2026-10-21, console at 460.8 MHz): 36u four full-screen passes (copy, edges, weights, blend) 3.96 ms a frame
// on average; 36v this form with the mask GREATER 2.79; 36w only the masked pixels writing (cleared targets, a discard,
// a masked copy back) 3.12: the edge pass is bound by its five samples (1.46 ms either way) and the copy pass added
// 0.27 ms, so 36y is 36v's form with 36w's LESS mask (the direction zcull works in) and its per-pass labels ("SMAA
// edges", "SMAA weights", "SMAA blend"). The same pixels each time: banjo_renut_diag_smaa_compare_s also runs SMAA itself
// in its frames (its own edge detection, the full-screen weights and blending) and counts the pixels where they differ.
#include "renut_engine/gpu/render/smaa.h"

#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>
#include <rex/filesystem.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/gpu_profile.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/render/textures.h"

#include "../thirdparty/smaa/AreaTex.h"
#include "../thirdparty/smaa/SearchTex.h"

#include "host_shaders/smaa_blend_ps.spirv.h"
#include "host_shaders/smaa_edge_ps.spirv.h"
#include "host_shaders/smaa_edge_ref_ps.spirv.h"
#include "host_shaders/smaa_vs.spirv.h"
#include "host_shaders/smaa_weights_high_ps.spirv.h"
#include "host_shaders/smaa_weights_ps.spirv.h"
#if defined(PLUME_DEKO3D)
#include "host_shaders/smaa_blend_ps.deko3d.h"
#include "host_shaders/smaa_edge_ps.deko3d.h"
#include "host_shaders/smaa_edge_ref_ps.deko3d.h"
#include "host_shaders/smaa_vs.deko3d.h"
#include "host_shaders/smaa_weights_high_ps.deko3d.h"
#include "host_shaders/smaa_weights_ps.deko3d.h"
#endif
#if defined(_WIN32)
#include "host_shaders/smaa_blend_ps.dxil.h"
#include "host_shaders/smaa_edge_ps.dxil.h"
#include "host_shaders/smaa_edge_ref_ps.dxil.h"
#include "host_shaders/smaa_vs.dxil.h"
#include "host_shaders/smaa_weights_high_ps.dxil.h"
#include "host_shaders/smaa_weights_ps.dxil.h"
#endif

namespace renut::gpu {
namespace {

constexpr plume::RenderFormat kEdgesFormat = plume::RenderFormat::R8G8_UNORM;
constexpr plume::RenderFormat kWeightsFormat = plume::RenderFormat::R8G8B8A8_UNORM;
constexpr plume::RenderFormat kMaskFormat = plume::RenderFormat::D16_UNORM;
// The mask: cleared to 0 (nothing to do), 1 on edge pixels, 0.5 on pixels only the blend can change. The masked passes
// draw at one depth (the viewport's depth range) with the test LESS, the usual direction, which the GPU's coarse depth
// culling (zcull) can apply to whole tiles left as cleared.
constexpr float kMaskClear = 0.0f;
constexpr float kWeightsDepth = 0.75f;  // passes 1: the edge pixels
constexpr float kBlendDepth = 0.25f;    // passes 1 and 0.5: every pixel the blend can change

enum class DepthUse { None, WriteAlways, TestLess };

// A texture of the passes: the image, its sampled view and bindless slot (colour), its layout.
struct PassTexture {
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  uint32_t slot = kInvalidDescriptor;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;

  void Release() {
    if (slot != kInvalidDescriptor)
      Video::FreeTextureSlotLocked(slot);
    slot = kInvalidDescriptor;
    view.reset();
    texture.reset();
    layout = plume::RenderTextureLayout::UNKNOWN;
  }
};

using PipelineMap = std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>>;  // by the target's format

struct Smaa {
  bool failed = false;  // a shader or pipeline could not be made: SMAA stays off
  uint64_t retry_frame = 0;  // sasrt-nx (2026-11-09 night; port 58): its textures could not be made: tried again then
  std::unique_ptr<plume::RenderShader> vs, edge_ps, weights_ps[2], blend_ps;
  std::unique_ptr<plume::RenderPipeline> weights_pipeline[2];
  PipelineMap edge_pipelines, blend_pipelines;
  PassTexture area, search;
  bool lookup_uploaded = false;
  // At the target's host size and format.
  uint32_t width = 0, height = 0;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  PassTexture src, edges, weights, mask;
  std::unique_ptr<plume::RenderFramebuffer> edge_framebuffer, weights_framebuffer;
  // The blend pass's framebuffer: the target with the mask, for the surface and generation it was made for.
  const HostSurface *target_surface = nullptr;
  uint64_t target_generation = 0;
  std::unique_ptr<plume::RenderFramebuffer> target_framebuffer;
  uint64_t last_frame = UINT64_MAX;
  // banjo_renut_diag_smaa_compare_s: the full-screen passes (no mask) into check textures, made on first use.
  std::unique_ptr<plume::RenderShader> check_edge_ps;
  std::unique_ptr<plume::RenderPipeline> check_edge_pipeline, check_weights_pipeline[2];
  PipelineMap check_blend_pipelines;
  PassTexture check_edges, check_weights, check;
  std::unique_ptr<plume::RenderFramebuffer> check_edges_framebuffer, check_weights_framebuffer, check_framebuffer;
};
Smaa g_smaa;

// The transitions of one point of the passes, recorded as one barriers() call.
struct Barriers {
  plume::RenderTextureBarrier list[6];
  uint32_t count = 0;
  void Add(PassTexture &t, plume::RenderTextureLayout layout) {
    if (t.layout != layout) {
      list[count++] = plume::RenderTextureBarrier(t.texture.get(), layout);
      t.layout = layout;
    }
  }
  void Add(HostSurface *surface, plume::RenderTextureLayout layout) {
    if (surface->layout != layout) {
      list[count++] = plume::RenderTextureBarrier(surface->texture.get(), layout);
      surface->layout = layout;
    }
  }
  void Record(plume::RenderCommandList *cmd, plume::RenderBarrierStages stage = plume::RenderBarrierStage::GRAPHICS) {
    if (count)
      cmd->barriers(stage, nullptr, 0, list, count);
    count = 0;
  }
};

std::unique_ptr<plume::RenderPipeline> MakePipeline(VideoState &s, plume::RenderShader *ps,
                                                    const plume::RenderFormat *formats, uint32_t count, bool rgb_only,
                                                    DepthUse depth, const char *name) {
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.utility_layout.get();
  desc.vertexShader = g_smaa.vs.get();
  desc.pixelShader = ps;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = count;
  for (uint32_t i = 0; i < count; ++i) {
    desc.renderTargetFormat[i] = formats[i];
    desc.renderTargetBlend[i] = plume::RenderBlendDesc::Copy();
  }
  if (rgb_only)
    desc.renderTargetBlend[0].renderTargetWriteMask =
        uint8_t(plume::RenderColorWriteEnable::RED) | uint8_t(plume::RenderColorWriteEnable::GREEN) |
        uint8_t(plume::RenderColorWriteEnable::BLUE);
  if (depth == DepthUse::None) {
    desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  } else {
    desc.depthTargetFormat = kMaskFormat;
    desc.depthEnabled = true;
    desc.depthWriteEnabled = depth == DepthUse::WriteAlways;
    desc.depthFunction = depth == DepthUse::WriteAlways ? plume::RenderComparisonFunction::ALWAYS
                                                        : plume::RenderComparisonFunction::LESS;
  }
  std::unique_ptr<plume::RenderPipeline> pipeline = s.device->createGraphicsPipeline(desc);
  if (pipeline)
    pipeline->setName(name);
  return pipeline;
}

bool MakeShaders(VideoState &s) {
  Smaa &m = g_smaa;
  m.vs = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(smaa_vs), "smaa_vs");
  m.edge_ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(smaa_edge_ps), "smaa_edge_ps");
  m.weights_ps[0] = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(smaa_weights_ps), "smaa_weights_ps");
  m.weights_ps[1] = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(smaa_weights_high_ps), "smaa_weights_high_ps");
  m.blend_ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(smaa_blend_ps), "smaa_blend_ps");
  if (!m.vs || !m.edge_ps || !m.weights_ps[0] || !m.weights_ps[1] || !m.blend_ps)
    return false;
  for (uint32_t i = 0; i < 2; ++i) {
    m.weights_pipeline[i] = MakePipeline(s, m.weights_ps[i].get(), &kWeightsFormat, 1, false, DepthUse::TestLess,
                                         "renut SMAA weights");
    if (!m.weights_pipeline[i])
      return false;
  }
  return true;
}

// A colour texture drawn into and sampled ('slot'), or the depth mask (no slot).
bool MakeTexture(VideoState &s, PassTexture &t, uint32_t width, uint32_t height, plume::RenderFormat format,
                 const char *name) {
  t.Release();
  const bool depth = format == kMaskFormat;
  t.texture = s.device->createTexture(depth ? plume::RenderTextureDesc::DepthTarget(width, height, format)
                                            : plume::RenderTextureDesc::ColorTarget(width, height, format));
  if (!t.texture)
    return false;
  t.texture->setName(name);
  if (depth)
    return true;
  t.view = t.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(format));
  if (!t.view)
    return false;
  t.slot = Video::AllocateTextureSlotLocked();
  if (t.slot == kInvalidDescriptor)
    return false;
  Video::SetTextureSlotLocked(t.slot, t.texture.get(), t.view.get());
  return true;
}

// A sampled texture with initial data (the lookup textures): no framebuffer, no target.
bool MakeLookup(VideoState &s, PassTexture &t, uint32_t width, uint32_t height, plume::RenderFormat format,
                const char *name) {
  t.Release();
  t.texture = s.device->createTexture(plume::RenderTextureDesc::Texture2D(width, height, 1, format));
  if (!t.texture)
    return false;
  t.texture->setName(name);
  t.view = t.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(format));
  if (!t.view)
    return false;
  t.slot = Video::AllocateTextureSlotLocked();
  if (t.slot == kInvalidDescriptor)
    return false;
  Video::SetTextureSlotLocked(t.slot, t.texture.get(), t.view.get());
  return true;
}

std::unique_ptr<plume::RenderFramebuffer> MakeFramebuffer(VideoState &s, std::initializer_list<plume::RenderTexture *> colors,
                                                          plume::RenderTexture *depth) {
  const plume::RenderTexture *targets[4] = {};
  uint32_t count = 0;
  for (plume::RenderTexture *c : colors)
    targets[count++] = c;
  return s.device->createFramebuffer(plume::RenderFramebufferDesc(targets, count, depth));
}

// SMAA's area (160 x 560, R8G8) and search (64 x 16, R8) textures, once. Rows padded to 256 bytes (D3D12's pitch).
bool UploadLookups(VideoState &s) {
  plume::RenderCommandList *cmd = s.command_list;
  struct Lookup {
    PassTexture &t;
    const unsigned char *bytes;
    uint32_t width, height, texel_bytes;
    plume::RenderFormat format;
  };
  const Lookup lookups[] = {
      {g_smaa.area, areaTexBytes, AREATEX_WIDTH, AREATEX_HEIGHT, 2, plume::RenderFormat::R8G8_UNORM},
      {g_smaa.search, searchTexBytes, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1, plume::RenderFormat::R8_UNORM},
  };
  cmd->setFramebuffer(nullptr);
  for (const Lookup &l : lookups) {
    const uint32_t row = l.width * l.texel_bytes;
    const uint32_t pitch = (row + 255u) & ~255u;
    UploadAllocation staging = s.uploads[s.frame].Allocate(uint64_t(pitch) * l.height, 512);
    if (!staging)
      return false;
    auto *dst = static_cast<uint8_t *>(staging.cpu);
    for (uint32_t y = 0; y < l.height; ++y)
      std::memcpy(dst + size_t(y) * pitch, l.bytes + size_t(y) * row, row);
    Barriers b;
    b.Add(l.t, plume::RenderTextureLayout::COPY_DEST);
    b.Record(cmd, plume::RenderBarrierStage::COPY);
    cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(l.t.texture.get(), 0, 0),
                           plume::RenderTextureCopyLocation::PlacedFootprint(staging.ref.ref, l.format, l.width,
                                                                             l.height, 1, pitch / l.texel_bytes,
                                                                             staging.ref.offset),
                           0, 0, 0, nullptr);
    b.Add(l.t, plume::RenderTextureLayout::SHADER_READ);
    b.Record(cmd);
  }
  return true;
}

bool Fail(const char *what) {
  g_smaa.failed = true;
  RGPU_WARN("[smaa] {} could not be made: banjo_renut_smaa stays off", what);
  return false;
}

bool Prepare(VideoState &s, HostSurface *surface, uint32_t width, uint32_t height) {
  Smaa &m = g_smaa;
  if (TextureFrameLocked() < m.retry_frame)
    return false;  // sasrt-nx (2026-11-09 night; port 58): see below
  if (!m.vs && !MakeShaders(s))
    return Fail("its shaders or pipelines");
  if (!m.area.texture &&
      (!MakeLookup(s, m.area, AREATEX_WIDTH, AREATEX_HEIGHT, plume::RenderFormat::R8G8_UNORM, "renut SMAA area") ||
       !MakeLookup(s, m.search, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, plume::RenderFormat::R8_UNORM, "renut SMAA search")))
    return Fail("the lookup textures");
  if (!m.lookup_uploaded) {
    if (!UploadLookups(s))
      return false;  // the upload ring is full this frame: the next one
    m.lookup_uploaded = true;
  }
  if (m.width != width || m.height != height || m.format != surface->format || !m.src.texture) {
    m.width = width;
    m.height = height;
    m.format = surface->format;
    m.check.Release();  // the comparison's: made again at the new size when wanted
    m.check_weights.Release();
    m.check_edges.Release();
    m.check_framebuffer.reset();
    m.check_weights_framebuffer.reset();
    m.check_edges_framebuffer.reset();
    if (!MakeTexture(s, m.src, width, height, surface->format, "renut SMAA source") ||
        !MakeTexture(s, m.edges, width, height, kEdgesFormat, "renut SMAA edges") ||
        !MakeTexture(s, m.weights, width, height, kWeightsFormat, "renut SMAA weights") ||
        !MakeTexture(s, m.mask, width, height, kMaskFormat, "renut SMAA mask")) {
      // sasrt-nx (2026-11-09 night; port 58; SASRT's run mem12: a memory emergency, 27 textures not made in a second):
      // out of memory for a moment, most likely: frames without SMAA for 60 frames, then tried again, not off for the
      // rest of the session
      m.src.Release();
      m.edges.Release();
      m.weights.Release();
      m.mask.Release();
      m.edge_framebuffer.reset();
      m.weights_framebuffer.reset();
      m.target_framebuffer.reset();
      m.width = m.height = 0;
      m.retry_frame = TextureFrameLocked() + 60;
      static uint32_t retry_logged = 0;
      if (retry_logged++ < 16)
        RGPU_WARN("[smaa] its textures could not be made ({}x{}): SMAA left out for 60 frames, then tried again",
                  width, height);
      return false;
    }
    m.edge_framebuffer = MakeFramebuffer(s, {m.edges.texture.get(), m.src.texture.get()}, m.mask.texture.get());
    m.weights_framebuffer = MakeFramebuffer(s, {m.weights.texture.get()}, m.mask.texture.get());
    m.target_framebuffer.reset();
    if (!m.edge_framebuffer || !m.weights_framebuffer)
      return Fail("its framebuffers");
    RGPU_INFO("[smaa] textures made for {}x{} (format {})", width, height, uint32_t(surface->format));
  }
  const uint32_t key = uint32_t(surface->format);
  if (!m.blend_pipelines[key]) {
    const plume::RenderFormat edge_formats[] = {kEdgesFormat, surface->format};
    m.edge_pipelines[key] =
        MakePipeline(s, m.edge_ps.get(), edge_formats, 2, false, DepthUse::WriteAlways, "renut SMAA edges");
    m.blend_pipelines[key] =
        MakePipeline(s, m.blend_ps.get(), &surface->format, 1, true, DepthUse::TestLess, "renut SMAA blend");
    if (!m.edge_pipelines[key] || !m.blend_pipelines[key])
      return Fail("its pipelines for the target's format");
  }
  if (m.target_surface != surface || m.target_generation != surface->generation || !m.target_framebuffer) {
    m.target_framebuffer = MakeFramebuffer(s, {surface->texture.get()}, m.mask.texture.get());
    m.target_surface = surface;
    m.target_generation = surface->generation;
    if (!m.target_framebuffer)
      return false;
  }
  return true;
}

void Draw(plume::RenderCommandList *cmd, plume::RenderFramebuffer *framebuffer, plume::RenderPipeline *pipeline,
          const UtilityPush &push, uint32_t width, uint32_t height, float depth = -1.0f) {
  cmd->setFramebuffer(framebuffer);
  cmd->setViewports(depth < 0.0f ? plume::RenderViewport(0.0f, 0.0f, float(width), float(height))
                                 : plume::RenderViewport(0.0f, 0.0f, float(width), float(height), depth, depth));
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));
  cmd->setPipeline(pipeline);
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// banjo_renut_diag_smaa_compare_s: every N seconds one frame's 3D image before SMAA and after it (the target), the HUD
// not drawn yet, and the same frame through SMAA itself (its own edge detection with its discard, the full-screen weights
// and blending: 36u's passes) into 'check', copied into
// readback buffers and written three frames later (the slot's fence waited by then): capturas/smaa_NNN_off.tga and
// _on.tga next to the NRO, and a log line with the pixels where the masked and the full-screen results differ (0: the
// masks are exact).
struct Compare {
  std::unique_ptr<plume::RenderBuffer> readback[3];  // before, after, after through the full-screen passes
  uint32_t width = 0, height = 0, row_texels = 0;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  uint64_t frame = 0;  // the frame the copies were recorded in; 0: none waiting
  std::chrono::steady_clock::time_point last{};
  uint32_t written = 0;
};
Compare g_compare;

bool CompareWanted(VideoState &s, uint32_t width, uint32_t height, plume::RenderFormat format) {
  const int32_t every = banjo::renut_gpu::SmaaCompareSeconds();
  if (every <= 0 || g_compare.frame)
    return false;
  // blur-nx (2026-11-12): the readback and its TGA files are 4 bytes a texel: 8-bit targets only (not RGBA16F).
  if (format != plume::RenderFormat::R8G8B8A8_UNORM && format != plume::RenderFormat::B8G8R8A8_UNORM)
    return false;
  const auto now = std::chrono::steady_clock::now();
  if (g_compare.last != std::chrono::steady_clock::time_point{} && now - g_compare.last < std::chrono::seconds(every))
    return false;
  Compare &c = g_compare;
  Smaa &m = g_smaa;
  if (!c.readback[0] || c.width != width || c.height != height) {
    c.row_texels = (width + 63) & ~63u;  // D3D12 copies rows of 256-byte multiples
    for (auto &r : c.readback) {
      r = s.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(uint64_t(c.row_texels) * height * 4));
      if (!r)
        return false;
    }
    c.width = width;
    c.height = height;
  }
  if (!m.check.texture) {
    if (!MakeTexture(s, m.check_edges, width, height, kEdgesFormat, "renut SMAA check edges") ||
        !MakeTexture(s, m.check_weights, width, height, kWeightsFormat, "renut SMAA check weights") ||
        !MakeTexture(s, m.check, width, height, format, "renut SMAA check"))
      return false;
    m.check_edges_framebuffer = MakeFramebuffer(s, {m.check_edges.texture.get()}, nullptr);
    m.check_weights_framebuffer = MakeFramebuffer(s, {m.check_weights.texture.get()}, nullptr);
    m.check_framebuffer = MakeFramebuffer(s, {m.check.texture.get()}, nullptr);
    if (!m.check_edges_framebuffer || !m.check_weights_framebuffer || !m.check_framebuffer)
      return false;
  }
  if (!m.check_edge_pipeline) {
    m.check_edge_ps = CreateHostShader(s.device.get(), RENUT_HOST_SHADER(smaa_edge_ref_ps), "smaa_edge_ref_ps");
    if (m.check_edge_ps)
      m.check_edge_pipeline =
          MakePipeline(s, m.check_edge_ps.get(), &kEdgesFormat, 1, false, DepthUse::None, "renut SMAA check edges");
    if (!m.check_edge_pipeline)
      return false;
  }
  const uint32_t key = uint32_t(format);
  if (!m.check_weights_pipeline[0]) {
    for (uint32_t i = 0; i < 2; ++i)
      m.check_weights_pipeline[i] = MakePipeline(s, m.weights_ps[i].get(), &kWeightsFormat, 1, false, DepthUse::None,
                                                 "renut SMAA check weights");
  }
  if (!m.check_blend_pipelines[key])
    m.check_blend_pipelines[key] =
        MakePipeline(s, m.blend_ps.get(), &format, 1, false, DepthUse::None, "renut SMAA check blend");
  if (!m.check_weights_pipeline[0] || !m.check_weights_pipeline[1] || !m.check_blend_pipelines[key])
    return false;
  c.format = format;
  c.last = now;
  return true;
}

void CopyToReadback(plume::RenderCommandList *cmd, plume::RenderTexture *texture, uint32_t which) {
  Compare &c = g_compare;
  const plume::RenderBox box(0, 0, int32_t(c.width), int32_t(c.height));
  cmd->copyTextureRegion(plume::RenderTextureCopyLocation::PlacedFootprint(c.readback[which].get(), c.format, c.width,
                                                                           c.height, 1, c.row_texels),
                         plume::RenderTextureCopyLocation::Subresource(texture), 0, 0, 0, &box);
}

// The waiting set, three frames after it was recorded: two 24-bit TGA files (top-left origin) and the check.
void WriteCompare(uint64_t frame) {
  Compare &c = g_compare;
  if (!c.frame || frame < c.frame + 3)
    return;
  c.frame = 0;
  const auto folder = rex::filesystem::GetExecutableFolder() / "capturas";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const bool rgba = c.format == plume::RenderFormat::R8G8B8A8_UNORM;
  const uint8_t *data[3] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    data[i] = static_cast<const uint8_t *>(c.readback[i]->map());
    if (!data[i]) {
      for (uint32_t j = 0; j < i; ++j)
        c.readback[j]->unmap();
      return;
    }
  }
  std::vector<uint8_t> pixels(size_t(c.width) * c.height * 3);
  for (uint32_t which = 0; which < 2; ++which) {
    for (uint32_t y = 0; y < c.height; ++y) {
      const uint8_t *row = data[which] + size_t(y) * c.row_texels * 4;
      uint8_t *out = pixels.data() + size_t(y) * c.width * 3;
      for (uint32_t x = 0; x < c.width; ++x) {
        out[x * 3 + 0] = row[x * 4 + (rgba ? 2 : 0)];  // TGA: blue, green, red
        out[x * 3 + 1] = row[x * 4 + 1];
        out[x * 3 + 2] = row[x * 4 + (rgba ? 0 : 2)];
      }
    }
    char name[64];
    std::snprintf(name, sizeof(name), "smaa_%03u_%s.tga", c.written, which ? "on" : "off");
    if (std::FILE *f = std::fopen((folder / name).string().c_str(), "wb")) {
      const uint8_t header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(c.width), uint8_t(c.width >> 8),
                                  uint8_t(c.height), uint8_t(c.height >> 8), 24, 0x20};
      std::fwrite(header, 1, sizeof(header), f);
      std::fwrite(pixels.data(), 1, pixels.size(), f);
      std::fclose(f);
    }
  }
  // The masked passes against the full-screen ones: red, green and blue (the target keeps its alpha).
  uint64_t differ = 0, changed = 0;
  for (uint32_t y = 0; y < c.height; ++y) {
    const size_t at = size_t(y) * c.row_texels * 4;
    for (uint32_t x = 0; x < c.width; ++x) {
      const uint8_t *a = data[1] + at + x * 4, *b = data[2] + at + x * 4, *o = data[0] + at + x * 4;
      differ += a[0] != b[0] || a[1] != b[1] || a[2] != b[2];
      changed += a[0] != o[0] || a[1] != o[1] || a[2] != o[2];
    }
  }
  for (uint32_t i = 0; i < 3; ++i)
    c.readback[i]->unmap();
  RGPU_INFO("[smaa] comparison {} written (capturas/smaa_{:03}_off.tga and _on.tga, {}x{}): {} pixels changed by "
            "SMAA; the optimised passes against SMAA itself: {} pixels differ",
            c.written, c.written, c.width, c.height, changed, differ);
  ++c.written;
}

}  // namespace

bool SmaaAfterPs(uint64_t ps_hash) {
  return banjo::renut_gpu::SmaaOn() && !g_smaa.failed && CompositePs(ps_hash);
}

bool CompositePs(uint64_t ps_hash) {
  static const std::vector<uint64_t> list = [] {
    std::vector<uint64_t> v;
    const std::string text = banjo::renut_gpu::SmaaAfterPsList();
    size_t at = 0;
    while (at < text.size()) {
      size_t end = text.find(',', at);
      if (end == std::string::npos)
        end = text.size();
      const std::string item = text.substr(at, end - at);
      if (!item.empty())
        v.push_back(std::stoull(item, nullptr, 16));
      at = end + 1;
    }
    return v;
  }();
  for (const uint64_t h : list)
    if (h == ps_hash)
      return true;
  return false;
}

bool SmaaRunLocked(HostSurface *surface) {
  VideoState &s = State();
  Smaa &m = g_smaa;
  // blur-nx (2026-11-12): RGBA16F too. Blur composites into its 1280x720 float target (the 360's 2_10_10_10_FLOAT
  // EDRAM, which Renut makes RGBA16F; pixel shader 91C0712EC807B3DF, the tone-mapped image before the HUD); the passes
  // already make their source copy and pipelines in the target's own format (edges and weights stay R8G8 / RGBA8).
  if (!surface || surface->depth || !surface->texture || surface->samples != plume::RenderSampleCount::COUNT_1 ||
      (surface->format != plume::RenderFormat::R8G8B8A8_UNORM &&
       surface->format != plume::RenderFormat::B8G8R8A8_UNORM &&
       surface->format != plume::RenderFormat::R16G16B16A16_FLOAT))
    return false;
  // Once a frame: a second composite in the same frame would soften the image twice.
  const uint64_t frame = TextureFrameLocked();
  if (m.last_frame == frame)
    return false;
  WriteCompare(frame);  // banjo_renut_diag_smaa_compare_s: a set recorded three frames ago
  const uint32_t width = uint32_t(surface->scale.Coord(int32_t(surface->width)));
  const uint32_t height = uint32_t(surface->scale.Coord(int32_t(surface->height)));
  if (!width || !height || !s.command_list || !Prepare(s, surface, width, height))
    return false;
  const uint32_t surface_slot = SurfaceSampleSlotLocked(surface);
  if (surface_slot == kInvalidDescriptor)
    return false;
  m.last_frame = frame;
  static const uint32_t kEdgesLabel = gpu_profile::Label("SMAA edges");
  static const uint32_t kWeightsLabel = gpu_profile::Label("SMAA weights");
  static const uint32_t kBlendLabel = gpu_profile::Label("SMAA blend");
  plume::RenderCommandList *cmd = s.command_list;
  const uint32_t quality = banjo::renut_gpu::SmaaQuality() >= 2 ? 1u : 0u;
  const uint32_t key = uint32_t(surface->format);
  const bool compare = CompareWanted(s, width, height, surface->format);

  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);

  // SMAA_RT_METRICS from scale and translate (smaa.hlsli).
  UtilityPush base{};
  base.scale[0] = 1.0f / float(width);
  base.scale[1] = 1.0f / float(height);
  base.translate[0] = float(width);
  base.translate[1] = float(height);
  base.sampler_index = kLinearClampSamplerSlot;

  // 1. Edges, the copy and the mask, from the target the composite just wrote (every pixel: no clears).
  gpu_profile::Mark(s, kEdgesLabel);
  Barriers b;
  b.Add(surface, plume::RenderTextureLayout::SHADER_READ);
  b.Add(m.edges, plume::RenderTextureLayout::COLOR_WRITE);
  b.Add(m.src, plume::RenderTextureLayout::COLOR_WRITE);
  b.Add(m.mask, plume::RenderTextureLayout::DEPTH_WRITE);
  b.Record(cmd);
  UtilityPush edge = base;
  edge.texture_index = surface_slot;
  edge.sampler_index = kPointClampSamplerSlot;  // the edge pass's point samples (smaa_edge_ps.hlsl)
  edge.param0 = float(banjo::renut_gpu::SmaaThreshold());
  Draw(cmd, m.edge_framebuffer.get(), m.edge_pipelines[key].get(), edge, width, height);

  // 2. Blending weights on the edge pixels (the rest stay as cleared: 0).
  gpu_profile::Mark(s, kWeightsLabel);
  b.Add(m.edges, plume::RenderTextureLayout::SHADER_READ);
  b.Add(m.weights, plume::RenderTextureLayout::COLOR_WRITE);
  b.Record(cmd);
  cmd->setFramebuffer(m.weights_framebuffer.get());
  cmd->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  UtilityPush weights = base;
  weights.texture_index = m.edges.slot;
  weights.param0 = std::bit_cast<float>(m.area.slot);
  weights.param1 = std::bit_cast<float>(m.search.slot);
  Draw(cmd, m.weights_framebuffer.get(), m.weights_pipeline[quality].get(), weights, width, height, kWeightsDepth);

  // 3. Neighbourhood blending of src into the game's target, on the pixels it can change.
  gpu_profile::Mark(s, kBlendLabel);
  b.Add(m.weights, plume::RenderTextureLayout::SHADER_READ);
  b.Add(m.src, plume::RenderTextureLayout::SHADER_READ);
  b.Add(surface, plume::RenderTextureLayout::COLOR_WRITE);
  b.Record(cmd);
  UtilityPush blend = base;
  blend.texture_index = m.src.slot;
  blend.param0 = std::bit_cast<float>(m.weights.slot);
  Draw(cmd, m.target_framebuffer.get(), m.blend_pipelines[key].get(), blend, width, height, kBlendDepth);

  // banjo_renut_diag_smaa_compare_s: SMAA itself (its own edge detection, the full-screen weights and blending of src,
  // the image as the composite left it) into check; then src (before), the target (after) and check read back.
  if (compare) {
    b.Add(m.check_edges, plume::RenderTextureLayout::COLOR_WRITE);
    b.Record(cmd);
    cmd->setFramebuffer(m.check_edges_framebuffer.get());
    cmd->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
    UtilityPush check_edge = edge;
    check_edge.texture_index = m.src.slot;  // the same pixels as the target before SMAA
    Draw(cmd, m.check_edges_framebuffer.get(), m.check_edge_pipeline.get(), check_edge, width, height);
    b.Add(m.check_edges, plume::RenderTextureLayout::SHADER_READ);
    b.Add(m.check_weights, plume::RenderTextureLayout::COLOR_WRITE);
    b.Record(cmd);
    UtilityPush check_weights = weights;
    check_weights.texture_index = m.check_edges.slot;
    Draw(cmd, m.check_weights_framebuffer.get(), m.check_weights_pipeline[quality].get(), check_weights, width, height);
    b.Add(m.check_weights, plume::RenderTextureLayout::SHADER_READ);
    b.Add(m.check, plume::RenderTextureLayout::COLOR_WRITE);
    b.Record(cmd);
    UtilityPush check = blend;
    check.param0 = std::bit_cast<float>(m.check_weights.slot);
    Draw(cmd, m.check_framebuffer.get(), m.check_blend_pipelines[key].get(), check, width, height);
    cmd->setFramebuffer(nullptr);
    b.Add(m.src, plume::RenderTextureLayout::COPY_SOURCE);
    b.Add(surface, plume::RenderTextureLayout::COPY_SOURCE);
    b.Add(m.check, plume::RenderTextureLayout::COPY_SOURCE);
    b.Record(cmd, plume::RenderBarrierStage::COPY);
    CopyToReadback(cmd, m.src.texture.get(), 0);
    CopyToReadback(cmd, surface->texture.get(), 1);
    CopyToReadback(cmd, m.check.texture.get(), 2);
    g_compare.frame = frame;
  }

  ++surface->writes;  // banjo_renut_resolve_skip: the target changed
  banjo::renut_gpu::CountSmaa();
  return true;
}

}  // namespace renut::gpu
