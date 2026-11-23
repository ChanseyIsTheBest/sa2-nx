/**
 * @file    gpu/render/surfaces.h
 * @brief   Host render targets behind guest D3DSurfaces.
 *
 * N&B's own D3DDevice_CreateSurface still builds the surface (and does its
 * EDRAM bookkeeping); the renderer attaches a host color or depth target to
 * the EDRAM view the surface describes (base, pitch, samples, format) the
 * first time a surface over it is bound, sized and formatted from the surface
 * header. Surfaces over the same view share the target, as they share EDRAM
 * on the GPU; content lives in it while any of them does, which is what the
 * game relies on between Clear, draws and Resolve.
 * All functions require State().mutex.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

#include <rex/types.h>

#include "renut_engine/gpu/render/render_scale.h"
#include "renut_engine/gpu/d3d/d3d.h"  // blur-nx (2026-11-10, port 76; its key 215): d3d::SurfaceDesc in StackHeaderSet

namespace renut::gpu {

// banjo-nx (2026-10-19, banjo_deko3d_depth_handover): a depth target's image and what is made over it (its generation,
// layout, sample and stencil views), which moves between a surface and a depth resolve destination that holds the
// surface's content (render/draw.cpp, "Depth hand-over"). 'views' are a destination's views of it, by component
// mapping (the fetch swizzle as the destination's own R32 views take it).
struct DepthImage {
  std::unique_ptr<plume::RenderTexture> texture;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  uint64_t generation = 0;
  std::unique_ptr<plume::RenderTextureView> sample_view;
  uint32_t sample_slot = UINT32_MAX;
  std::unique_ptr<plume::RenderTextureView> stencil_view;
  uint32_t stencil_slot = UINT32_MAX;
  struct View {
    plume::RenderComponentMapping mapping;
    std::unique_ptr<plume::RenderTextureView> view;
    uint32_t slot = UINT32_MAX;
  };
  std::vector<View> views;
  // The target it was made for or last taken from: a surface takes it back only when these are its own.
  uint32_t host_width = 0, host_height = 0;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  plume::RenderSampleCounts samples = plume::RenderSampleCount::COUNT_1;
  u32 format_word = 0;
};

struct HostSurface {
  u32 guest_va = 0;  // the first surface over the view
  // Header words of the surface over the view bound most recently.
  u32 size_word = 0;
  u32 info_word = 0;
  u32 format_word = 0;
  u32 color_info_word = 0;  // halo3-nx (838ea8c587): RB_COLOR_INFO, the guest format (gamma) and colour exponent bias
  bool sasrt_fixed16 = false;  // sasrt-nx (2026-10-20): a 16-bit fixed-point colour target (k_16_16, k_16_16_16_16)
  // sasrt-nx (2026-10-20, the EDRAM depth restore): g_surface_write_seq at this surface's last write, and for a depth surface
  // the colour surface's write it was last restored from (RestoreDepthFromColorLocked).
  uint64_t write_seq = 0;
  uint64_t restored_from_seq = 0;
  // sasrt-nx (2026-11-09, sasrt_rt_restore_candidates; port 54): a depth target's colour targets over its EDRAM base and
  // pitch that could be its restore's source, as of the target table's epoch 'sasrt_candidates_epoch' (0: not found yet).
  std::vector<HostSurface *> sasrt_restore_candidates;
  uint64_t sasrt_candidates_epoch = 0;
  // sasrt-nx (2026-11-20, sasrt_rt2_color_alias_memo): ColorAliasesLocked's answer as of the target table's epoch.
  mutable uint64_t sasrt_alias_epoch = 0;
  mutable std::vector<HostSurface *> sasrt_aliases;
  // halo3-nx (2026-11-03): when it was last bound to draw to, and what of another format's target over the same EDRAM view it
  // holds (SyncColorViewLocked).
  uint64_t bound_serial = 0;
  uint64_t sasrt_used_frame = 0;  // sasrt-nx (2026-11-09, sasrt_retire_idle_targets_frames; port 58): last looked up
  const HostSurface *synced_view = nullptr;
  uint64_t synced_writes = 0;

  // In guest pixels; the host target is 'scale' times that on each axis
  // (ResolutionScale when it was built).
  uint32_t width = 0;
  uint32_t height = 0;
  RenderScale scale;
  bool depth = false;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  plume::RenderSampleCounts samples = plume::RenderSampleCount::COUNT_1;

  std::unique_ptr<plume::RenderTexture> texture;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  uint64_t generation = 0;  // bumped on rebuild, part of framebuffer keys
  // banjo-nx (2026-10-19, banjo_renut_resolve_skip): bumped by every draw that can write it (a colour write mask; for
  // depth, a depth write or stencil writes) and every clear of it; part of a resolve's signature.
  uint64_t writes = 0;
  // blur-nx (2026-11-10, port 76; its key 216: blur_renut_sample_grid_resolve): g_surface_write_seq at the last draw into
  // it (not its clears).
  uint64_t blur_draw_seq = 0;
  // Port 179 (from fnxf_xiii, 2026-10-31; banjo_renut_edram_carry, off): the frame a colour target was first drawn in and
  // the frame it was last cleared in, and the order of those writes (render/draw.cpp CarryEdramBeforeDrawLocked).
  uint64_t carry_drawn_frame = UINT64_MAX, carry_cleared_frame = UINT64_MAX;
  uint64_t carry_order = 0;
  // vivapinata-nx (2026-11-11, vivapinata_resolve_from_sibling / A/B 87): the last multisampled colour resolve of this target
  // that the API drew (ResolveStateLocked): its destination, that destination's resolve signature then, this target's
  // generation and writes then, and the rect in host pixels (the destination's texels at the same coordinates). A later
  // resolve of the unchanged target into another destination copies from there: the same texels, one sample read.
  // Clears since (rect_clears, and their host rects, up to four) do not end it: a later resolve outside them still finds
  // the same pixels (EndTiling's tiles resolve and clear one after the other).
  struct MsSibling {
    uint32_t dest_va = 0;
    uint64_t dest_sig = 0;
    uint64_t generation = 0, writes = 0, clears = 0;
    int32_t left = 0, top = 0, right = 0, bottom = 0;
    uint32_t cleared_count = 0;
    int32_t cleared[4][4] = {};  // left, top, right, bottom (host pixels)
  } ms_sibling;
  uint64_t rect_clears = 0;  // vivapinata-nx (2026-11-11): the colour clears of it (each also counts in writes)

  // The view and bindless slot shaders read the target through (depth
  // resolves); made on first use, see SurfaceSampleSlotLocked.
  std::unique_ptr<plume::RenderTextureView> sample_view;
  uint32_t sample_slot = UINT32_MAX;
  // banjo-nx (2026-10-18): a depth/stencil target's stencil view and its slot (SurfaceStencilSlotLocked).
  std::unique_ptr<plume::RenderTextureView> stencil_view;
  uint32_t stencil_slot = UINT32_MAX;
  // banjo-nx (2026-10-19, banjo_deko3d_depth_handover): the current image's views for destinations that sample it
  // (DepthImage::views; they go with the image), how many destinations hold its content, and whether anything has
  // used its stencil (a draw with the stencil test, a stencil clear): a clear of the depth alone hands the image over
  // only when nothing has. A stencil nothing writes is undefined either way (a depth resolve's words read it as is).
  std::vector<DepthImage::View> handover_views;
  uint32_t handed = 0;
  bool stencil_used = false;
  // banjo-nx (2026-10-28, banjo_renut_clear_repeat; from sa2-nx's sa2_renut_clear_repeat, credit sa2-nx; render/draw.cpp
  // ClearTargets): the last whole clear's values and the image (generation) and writes it left; a whole clear of the same
  // values while these still hold writes nothing new.
  bool clear_known = false;
  uint64_t clear_generation = 0, clear_writes = 0;
  float clear_color[4] = {};
  bool clear_depth_known = false, clear_stencil_known = false;
  float clear_depth = 0.0f;
  uint32_t clear_stencil = 0;
  // vivapinata-nx (2026-11-04, the white fog at the cuts, ColorEdramWipedLocked): a clear through another layout wiped this
  // target's EDRAM on the console. Only a resolve can tell: one before anything writes here clears first (render/draw.cpp).
  bool edram_wiped = false;
  uint64_t edram_wiped_generation = 0, edram_wiped_writes = 0;
  float edram_wiped_color[4] = {};
  // blur-nx (2026-11-10, port 76; its key 218: blur_renut_wipe_overwritten): g_surface_write_seq at the wipe.
  uint64_t blur_wiped_seq = 0;
  // blur-nx (2026-11-11, port 76; its key 220: blur_renut_edram_inherit): g_blur_color_write_last when this target last
  // looked for other views' writes over its EDRAM (InheritEdramBeforeDrawLocked, render/draw.cpp): none since, it need
  // not look again.
  uint64_t blur_inherit_checked = 0;

  // gpu_profile labels of draws with it as the first target: color only,
  // color with depth, depth only. Made on first use.
  uint32_t profile_labels[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
};

// Render resolution: every host render target, and every texture a resolve
// writes, is this many times its guest size on each axis (1 = the game's own
// 720p, 3 = 2160p, 2/3 = 480p). The renderer works in guest pixels and scales
// where it records host viewports, scissors, clears and copies; shaders see a
// scaled texture's guest size (the scale is in its descriptor index, see
// kResolutionScaleShift) and the guest pixel position.
// From the renut_resolution cvar, latched once per frame by
// LatchResolutionScaleLocked (at Swap) so a frame never mixes scales. Targets
// and resolve destinations of another scale are rebuilt on their next use.
RenderScale ResolutionScale();
void LatchResolutionScaleLocked();
// banjo-nx (2026-10-28, GPU block, banjo_renut_dynamic_height_handheld): a frame's GPU time (render thread), which
// decides the handheld height the next LatchResolutionScaleLocked takes.
void NoteGpuFrameForDynamicResolution(double gpu_ms);

// banjo-nx (2026-10-21, internal resolution, the user's request: banjo_renut_render_height_handheld / _docked): on
// deko3d the 3D image is drawn at the internal resolution (ResolutionScale: 648p handheld, 900p docked with the tomls'
// keys) and the HUD at the display's (UiScale: 720p, 1080p docked with banjo_renut_display_height_docked). Right after
// the frame's final composite (and SMAA) render/ui_scale.cpp scales the composite's target into its UI target
// (UiSurfaceLocked) and redirects the rest of the frame there: from then until the Swap, every lookup of the
// composite's target (any surface over its EDRAM view) answers the UI target, so the HUD's draws, clears and the resolve
// to the front buffer use it. Frames without that composite for a while (menus, loading screens) are drawn at the
// display's resolution whole (ResolutionScale = UiScale).
RenderScale UiScale();
// The composite's target at the display's resolution: made (or made again) for 'scene', colour only, same format and
// samples, not among the targets of the EDRAM views (it is no view's); nullptr when it cannot be made.
HostSurface *UiSurfaceLocked(HostSurface *scene);
// From now to the Swap, lookups of the target UiSurfaceLocked was last given answer its UI target.
void RedirectToUiSurfaceLocked();
// The same for the depth target bound with the composite's: a draw after the composite that tests depth (the menus' 3D
// icons on their item bars) gets a depth target at the display's resolution, filled from the scene's
// (render/ui_scale.cpp, UiDepthAfterCompositeLocked) the first time in the frame.
HostSurface *UiDepthSurfaceLocked(HostSurface *scene_depth);
void RedirectDepthToUiLocked();
bool UiRedirectedLocked();
// The frame's final composite was drawn (gameplay: the 3D image is at the internal resolution next frame).
void NoteCompositeLocked();
// The console's mode on the Switch (Reverse-NX obeyed); false elsewhere.
bool ConsoleDocked();
// banjo-nx (2026-10-21): the internal heights of handheld and docked mode as the shaders read them (SharedConstants
// pad_1352[1], v[84].w: handheld | docked << 16; render_scale.h codes 4 and 6).
uint32_t InternalHeightsWord();
// rr6-nx (2026-11-02, rr6_depth_covers_colour): the depth target at least 'width' x 'height' (surfaces.cpp).
HostSurface *GrowDepthToColourLocked(u32 surface_va, HostSurface *depth, uint32_t width, uint32_t height);
// sasrt-nx (2026-10-20, the EDRAM depth restore): the order of surface writes (HostSurface::write_seq), and the single-sample
// 32-bit colour target over the EDRAM base and pitch of depth target 'depth' written after both its own last write and the
// write it was last restored from, or nullptr.
extern uint64_t g_surface_write_seq;
// blur-nx (2026-11-11, port 76; its key 220: blur_renut_edram_inherit): the write_seq of the latest write to any colour
// target (draws, clears, view syncs and inheritances; not depth): a target whose own write_seq and blur_inherit_checked
// are both at least this has no other view's write over its EDRAM to take.
extern uint64_t g_blur_color_write_last;
// blur-nx (2026-11-11, port 76; its key 220): a colour target's EDRAM span for edram_inherit.hlsli (base, tiles across,
// samples across and down, 40-sample tiles packed as its g_Push.scale fields say) and its rows of tiles; false for a
// depth target or none.
bool EdramInheritSpanLocked(const HostSurface *surface, u32 &bits, u32 &rows);
// blur-nx (2026-11-11, port 76; its key 220): the other colour targets whose EDRAM tiles meet 'target''s and that were
// written after it (write_seq), oldest first, with the same EDRAM format and at the guest's scale like it; 'skipped'
// counts the ones over it written after it that are of another format or scale (not taken).
void EdramInheritSourcesLocked(const HostSurface *target, std::vector<HostSurface *> &out, u32 &skipped);
HostSurface *DepthRestoreSourceLocked(const HostSurface *depth);

// The host target for the D3DSurface at 'surface_va', created or rebuilt as
// needed. nullptr for 0 or when the target cannot be created.
HostSurface *GetHostSurfaceLocked(u32 surface_va);
// blur-nx (2026-11-10, port 76; its key 215: blur_renut_stack_surface_headers): the game binds the surface at surface_va
// (any game thread): a header on the stack is kept as it is now, for the render thread's later reads.
void NoteSurfaceHeader(u32 surface_va);
// blur-nx (2026-11-10, port 76; its key 215): the headers of the stack surfaces bound as render targets and depth when a
// draw, clear or resolve is queued or recorded (game thread), and the render thread's use of them while it replays that
// operation: the game thread runs ahead, and a stack slot can hold another surface's header by the time the operation
// is replayed (one global copy per address gave a clear the wrong EDRAM).
struct StackHeaderSet {
  u32 count = 0;
  u32 va[5] = {};
  d3d::SurfaceDesc desc[5] = {};
};
void CaptureStackHeaders(const u8 *device, StackHeaderSet &out);
void SetReplayStackHeaders(const StackHeaderSet *set);
// blur-nx (2026-11-10, port 76; its key 216: blur_renut_sample_grid_resolve): for a 4x or 2x colour surface, the
// single-sampled target over the same EDRAM base with the same host format and scale whose pixels are its samples (4x:
// twice the pitch; 2x: the same pitch, twice the rows), or nullptr.
HostSurface *SampleGridAliasLocked(const HostSurface *surface);
// blur-nx (2026-11-10, port 76; its key 218: blur_renut_wipe_overwritten): whether another colour target whose EDRAM
// tiles meet this one's was written (drawn or cleared) after write sequence 'seq'.
bool OverlapWrittenSinceLocked(const HostSurface *surface, uint64_t seq);
// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_chain_reuse): banjo-nx's surface epoch (bumped by every target built, retired,
// unused or rebound, the resolution scale latched and the tiling extent set).
uint64_t SurfaceEpochLocked();
// banjo-nx (2026-10-20, from sa2-nx): banjo-nx's framebuffer epoch (bumped when a framebuffer is retired).
uint64_t FramebufferEpochLocked();

// Only if one exists already.
HostSurface *FindHostSurfaceLocked(u32 surface_va);
// renut-ultimate (2026-11-16, port 145; tw2de_nx's): kept unused targets past their time released (once a frame;
// banjo_renut_keep_unused_targets).
void SweepUnusedTargetsLocked();

// The other color targets over the same EDRAM view as 'surface' (base tile,
// pitch, samples and scale), in another host format: on the console one
// memory, so what a clear writes through one is what the others then hold.
void ColorAliasesLocked(const HostSurface *surface, std::vector<HostSurface *> &out);

// vivapinata-nx (2026-11-04, the white fog at the cuts): the other color targets at the same EDRAM base in another layout (samples,
// pitch) whose every tile a clear of 'rect' (guest pixels) of 'surface' covers. On the console that memory then holds the clear.
// blur-nx (2026-11-10, port 76; its key 218: blur_renut_wipe_overwritten, on): exactly, by tile spans: a target whose every
// tile is one the clear fills whole, at any base (off: the same base tile and a tile count no greater than the clear's).
void ColorEdramWipedLocked(const HostSurface *surface, const plume::RenderRect &rect, std::vector<HostSurface *> &out);

// Predicated tiling (D3DDevice_BeginTiling .. EndTiling): the game draws the
// frame into surfaces the size of one EDRAM tile, and the GPU replays the
// draws once per tile with the tile's screen offset, resolving each tile into
// its part of the destination. The host has no EDRAM to fit, so a surface
// used while tiling is backed by a target covering every tile (and keeps that
// size), which the draws and the per-tile resolves address in screen space.
// 'width' x 'height' is the bottom-right corner of the tile rects; 0 x 0 ends
// tiling.
void SetTilingExtentLocked(uint32_t width, uint32_t height);

// The surface no longer uses its target (guest D3DResource_Destroy); the
// target is retired when no surface over its view remains.
void ReleaseHostSurfaceLocked(u32 surface_va);

// Bindless slot of the surface's target (the depth plane for depth targets,
// multisampled when the target is), or kInvalidDescriptor.
uint32_t SurfaceSampleSlotLocked(HostSurface *surface);
// banjo-nx (2026-10-18): its stencil view's slot (uint), or kInvalidDescriptor.
uint32_t SurfaceStencilSlotLocked(HostSurface *surface);

// Moves 'surface' into 'layout' with a barrier on the open command list.
void TransitionSurfaceLocked(HostSurface *surface, plume::RenderTextureLayout layout);

// For the memory report: host render targets and the video memory they take
// (estimated from format, size and samples).
void SurfaceMemoryLocked(size_t &targets, uint64_t &bytes);

// banjo-nx (2026-10-19): the live surface with this generation, or nullptr (rebuilt or gone since).
HostSurface *SurfaceByGenerationLocked(uint64_t generation);

// banjo-nx (2026-10-19, banjo_deko3d_depth_handover): exchanges the surface's image (its texture, generation, layout
// and views) with 'image'. Framebuffers are cached by surface and generation, so each image keeps its own.
void SwapDepthImageLocked(HostSurface &surface, DepthImage &image);
// A new image of the surface's size, format and samples for SwapDepthImageLocked; false when it cannot be made.
bool MakeDepthImageLocked(const HostSurface &surface, DepthImage &image);
// Retires the image, its views and the framebuffers made over it (deferred past in-flight frames).
void RetireDepthImageLocked(DepthImage &image);
// Moves the image into 'layout' with a barrier on the open command list.
void TransitionDepthImageLocked(DepthImage &image, plume::RenderTextureLayout layout);
// The slot of a view of the image with 'mapping' (made on first use, kept in 'views'), or kInvalidDescriptor.
uint32_t DepthImageViewSlotLocked(plume::RenderTexture *texture, plume::RenderFormat format,
                                  std::vector<DepthImage::View> &views, const plume::RenderComponentMapping &mapping);
// render/draw.cpp: a depth target is about to go; the destinations that still hold its content get it first.
void DepthHandOverBeforeRetireLocked(HostSurface *surface);

// Framebuffer over up to 4 color targets plus depth; cached per target set.
plume::RenderFramebuffer *GetFramebufferLocked(HostSurface *const *colors, uint32_t color_count,
                                               HostSurface *depth);

} // namespace renut::gpu
