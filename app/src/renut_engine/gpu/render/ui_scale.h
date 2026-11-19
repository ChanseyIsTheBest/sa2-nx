// banjo-nx (2026-10-21, internal resolution, the user's request): the 3D image at the internal resolution, the HUD at the
// display's (ui_scale.cpp).
#pragma once

namespace renut::gpu {

struct HostSurface;

// Right after the frame's final composite into 'surface' (and SMAA): notes the composite (gameplay frames draw the 3D
// image at the internal resolution) and, when the surface is at another scale than the display's, scales its image into
// its UI target (surfaces.h, UiSurfaceLocked) and redirects the rest of the frame there. True when it recorded the pass:
// the caller binds its targets, pipeline, layout and viewport again.
bool UiScaleAfterCompositeLocked(HostSurface *surface);

// After that, the first draw of the frame that tests depth (or stencil) with the scene's depth target bound (the menus'
// 3D icons on their item bars, drawn after the composite): fills the UI depth target at the display's resolution from
// it (nearest texel; the stencil 0) and redirects the rest of the frame's lookups of it there. The UI depth target, or
// nullptr (no redirect this frame, a multisampled depth target, ...). The caller binds its state again.
HostSurface *UiDepthAfterCompositeLocked(HostSurface *depth);

}  // namespace renut::gpu
