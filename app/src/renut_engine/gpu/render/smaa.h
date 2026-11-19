// banjo-nx (2026-10-21, banjo_renut_smaa): SMAA 1x on the frame's final composite, before the HUD (smaa.cpp).
#pragma once

#include <cstdint>

namespace renut::gpu {

struct HostSurface;

// Whether a draw with this pixel shader is the frame's final composite (banjo_renut_smaa_after_ps), with
// banjo_renut_smaa on.
bool SmaaAfterPs(uint64_t ps_hash);
// banjo-nx (2026-10-21): whether it is the frame's final composite (banjo_renut_smaa_after_ps), SMAA on or not (the
// internal resolution's upscale follows it: render/ui_scale.cpp).
bool CompositePs(uint64_t ps_hash);

// Records SMAA on 'surface' (an 8-bit RGBA colour target, single-sampled) on the render thread's command list: a copy
// of the image, edge detection, blending weights, neighbourhood blending written back into the surface (its alpha
// kept). Once a frame (TextureFrameLocked). True when it recorded the passes: the caller binds its targets, pipeline,
// layout and viewport again (they are the passes' now).
bool SmaaRunLocked(HostSurface *surface);

}  // namespace renut::gpu
