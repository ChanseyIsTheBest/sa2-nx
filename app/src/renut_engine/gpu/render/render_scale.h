/**
 * @file    gpu/render/render_scale.h
 * @brief   Render resolution: host pixels per guest pixel.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace renut::gpu {

// Host pixels per guest pixel of a render target, and of what resolves from
// it, as one of a fixed set of ratios: the render resolutions
// (renut_resolution: 1x, 480p, 1080p, 1440p, 4K) and the targets that render
// finer than the rest (graphics_options.h: shadow maps at 4x, ambient
// occlusion at twice the resolution: 4/3, 6x). Code 0 is 1x, the scale of all
// content that is not rendered (guest textures). Shaders get the code in the
// descriptor index (kResolutionScaleShift) and have the same factors
// (renutTextureScale in shader_common.h).
// banjo-nx (2026-10-21, internal resolution, the user's request): on deko3d codes 4 and 6 are the internal resolutions
// of handheld and docked mode instead of 3 (4K) and 6 (ambient occlusion at twice 4K), which the Switch never draws: any
// height (banjo_renut_render_height_handheld / _docked), height / 720 reduced, set once by LatchResolutionScaleLocked
// (SetInternalHeights) before any target is made. The shaders read the same heights from SharedConstants (v[84].w:
// handheld | docked << 16; tools/deko3d_glsl_opt.py pass scalecodes). The PC's back ends keep Renut's table (their
// shaders are made from shader_common.h).
struct RenderScale {
  static constexpr uint32_t kCount = 8;
#if defined(PLUME_DEKO3D)
  static inline uint32_t kNumerators[kCount] = {1, 2, 3, 2, 9, 4, 5, 4};
  static inline uint32_t kDenominators[kCount] = {1, 3, 2, 1, 10, 1, 4, 3};
  // sasrt-nx (2026-11-12, batch C/D, item C13; port 93): Factor()'s quotient per code, kept with the ratios (SetRatio:
  // the same single float division, done when a ratio is set instead of at every call; the initial ones folded the
  // same way, round to nearest).
  static inline float kFactors[kCount] = {float(1) / float(1), float(2) / float(3), float(3) / float(2),
                                          float(2) / float(1), float(9) / float(10), float(4) / float(1),
                                          float(5) / float(4), float(4) / float(3)};
  static void SetRatio(uint32_t code, uint32_t numerator, uint32_t denominator) {
    kNumerators[code] = numerator;
    kDenominators[code] = denominator;
    kFactors[code] = float(numerator) / float(denominator);
  }
#else
  static constexpr uint32_t kNumerators[kCount] = {1, 2, 3, 2, 3, 4, 6, 4};
  static constexpr uint32_t kDenominators[kCount] = {1, 3, 2, 1, 1, 1, 1, 3};
#endif
  // banjo-nx (2026-10-21): the codes of the internal resolutions (deko3d; on the PC 3x and 6x, as Renut's).
  static constexpr uint32_t kHandheldCode = 4, kDockedCode = 6;

  uint32_t code = 0;

#if defined(PLUME_DEKO3D)
  float Factor() const { return kFactors[code]; }  // sasrt-nx (2026-11-12, batch C/D, item C13; port 93)
#else
  float Factor() const { return float(kNumerators[code]) / float(kDenominators[code]); }
#endif
  bool operator==(const RenderScale &) const = default;

  // A guest pixel edge in host pixels, to the nearest: rects that share an
  // edge in guest pixels share it in host pixels.
  // sasrt-nx (2026-11-12, batch C/D, item D4; port 93): in integers, round half away from zero of guest * n / d: what
  // lround of the double quotient gave (its error, under 2^-20, cannot cross a .5, as a quotient that is not a half is
  // at least 1/(2d) from one), without the libm call and the double division. sasrt-nx checked 675 ratios x 2,451
  // values (1.65 M cases, random 32-bit guests among them): 0 differences.
  int32_t Coord(int32_t guest) const {
    const int64_t n = kNumerators[code], d = kDenominators[code];
    const int64_t a = (guest < 0 ? -int64_t(guest) : int64_t(guest)) * n;
    const int64_t q = (2 * a + d) / (2 * d);
    return int32_t(guest < 0 ? -q : q);
  }

  // A guest size in host pixels, 1 at least.
  uint32_t Size(uint32_t guest) const {
    return std::max<uint32_t>(uint32_t(Coord(int32_t(guest))), 1u);
  }

  // The scale of ratio numerator / denominator, if it is one of them.
  static bool FromRatio(uint32_t numerator, uint32_t denominator, RenderScale &out) {
    for (uint32_t c = 0; c < kCount; ++c) {
      if (uint64_t(numerator) * kDenominators[c] == uint64_t(kNumerators[c]) * denominator) {
        out.code = c;
        return true;
      }
    }
    return false;
  }

  // This scale times 'multiplier', or this one when that is not among them.
  RenderScale Times(uint32_t multiplier) const {
    RenderScale out = *this;
    FromRatio(kNumerators[code] * multiplier, kDenominators[code], out);
    return out;
  }
};

} // namespace renut::gpu
