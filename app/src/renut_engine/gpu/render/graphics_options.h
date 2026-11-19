/**
 * @file    gpu/render/graphics_options.h
 * @brief   Graphics quality beyond the console's (Nuts&Bolts/Graphics):
 *          shadow map and ambient occlusion resolution, anisotropic
 *          filtering.
 *
 * - renut_shadow_resolution: the shadow maps' render scale. The sun's two
 *   cascades are 1024x1024 and 768x768 D24S8 targets (512x512 in split
 *   screen; sub_82267458), and the local lights render 512x512 D24S8 depth
 *   converted in a 512x512 16_16_FLOAT target into a 2048x2048 atlas
 *   (sub_82380818); by default they follow the render resolution.
 * - renut_ambient_occlusion: the SSAO pass (sub_8232E488) renders at half
 *   the screen, into 640x360 targets at EDRAM base 1864; Full renders it at
 *   the screen's resolution.
 * - renut_anisotropic_filtering: the least a texture's linear minification
 *   filters anisotropically.
 * - renut_post_process_precision: the 8_8_8_8 targets of half the screen or
 *   less (bloom, depth of field, SSAO) as 16-bit unorm on the host. The bloom
 *   (bright pass at 640x360 from the 8-bit scene, then 320x180 down to 80x45,
 *   added at weights 1, 1.2, 1.6 and 2) carries a faint glow in 1/255 steps;
 *   upscaled to the screen they show as blobs that slide over dark areas as
 *   the camera moves. A resolve's destination takes its source's host format,
 *   so the passes reading them see the finer values too.
 */
#pragma once

#include <cstdint>

#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/render/format.h"
#include "renut_engine/gpu/render/render_scale.h"

namespace renut::gpu {

// Reads the options; the rest use what it read. Once a frame, with the render
// resolution (LatchResolutionScaleLocked), so a frame never mixes them.
void LatchGraphicsOptionsLocked();

// The render scale of the target 'desc' describes, given the render
// resolution's.
RenderScale SurfaceRenderScale(const d3d::SurfaceDesc &desc, RenderScale resolution);

// The host format of the color target 'desc' describes.
plume::RenderFormat SurfaceColorFormat(const d3d::SurfaceDesc &desc);

// The anisotropy linear minification gets at least (1 = the game's own).
uint32_t MinAnisotropy();
// banjo-nx (2026-10-28, GPU block): banjo_renut_anisotropy_cap (0: the game's own).
uint32_t AnisotropyCap();
// banjo-nx (2026-10-28, GPU block): dynamic resolution's keys (render/surfaces.cpp).
bool DynamicResolutionOn();
int32_t DynamicHeight(bool docked, bool high);  // the range's ends, as set (not rounded)
double DynamicGpuMs();  // banjo-nx (2026-10-28): a decimal key (34.5)
double DynamicUpSeconds();  // banjo-nx (2026-10-28): banjo_renut_dynamic_up_s (1 s)
int32_t DynamicTestSeconds();
int32_t DynamicSteps();  // banjo-nx (2026-10-28, night_gpu5): banjo_renut_dynamic_steps, the heights between the ends
int32_t DynamicStepLines(bool docked);  // sasrt-nx (2026-11-09): sasrt_dynamic_lines_handheld / _docked (the grid)

} // namespace renut::gpu
